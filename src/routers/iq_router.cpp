// $Id$

/*
 Copyright (c) 2007-2015, Trustees of The Leland Stanford Junior University
 All rights reserved.

 Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:

 Redistributions of source code must retain the above copyright notice, this 
 list of conditions and the following disclaimer.
 Redistributions in binary form must reproduce the above copyright notice, this
 list of conditions and the following disclaimer in the documentation and/or
 other materials provided with the distribution.

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS " aS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE 
 DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE LIABLE FOR
 ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND ON
 ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/



#include "iq_router.hpp"

#include <string>
#include <sstream>
#include <iostream>
#include <iomanip>
#include <cstdlib>
#include <cassert>
#include <limits>

#include "../globals.hpp"
#include "../random_utils.hpp"
#include "../vc.hpp"
#include "../routefunc.hpp"
#include "../outputset.hpp"
#include "../buffer.hpp"
#include "../buffer_state.hpp"
#include "../arbiters/roundrobin_arb.hpp"
#include "../allocators/allocator.hpp"
#include "../power/switch_monitor.hpp"
#include "../power/buffer_monitor.hpp"
#include "../flitchannel.hpp"
#include "../routers/router.hpp"


int print_flit = -1;
IQRouter::IQRouter(Configuration const &config, Module *parent,
                   string const &name, int id, int inputs, int outputs, int is_hub)
    : Router(config, parent, name, id, inputs, outputs, is_hub), _active(false)
{
  _packet_size = config.GetInt("packet_size");
  _use_read_reply = config.GetInt("use_read_write");
  _read_reply_size = config.GetInt("read_reply_size");
  _write_reply_size = config.GetInt("write_reply_size");

  _vcs = config.GetInt("num_vcs");

  _vc_busy_when_full = (config.GetInt("vc_busy_when_full") > 0);
  _vc_prioritize_empty = (config.GetInt("vc_prioritize_empty") > 0);
  _vc_shuffle_requests = (config.GetInt("vc_shuffle_requests") > 0);

  _speculative = (config.GetInt("speculative") > 0);
  _spec_check_elig = (config.GetInt("spec_check_elig") > 0);
  _spec_check_cred = (config.GetInt("spec_check_cred") > 0);
  _spec_mask_by_reqs = (config.GetInt("spec_mask_by_reqs") > 0);

  _routing_delay = config.GetInt("routing_delay");
  _vc_alloc_delay = config.GetInt("vc_alloc_delay");
  if (!_vc_alloc_delay)
  {
    Error("VC allocator cannot have zero delay.");
  }
  _sw_alloc_delay = config.GetInt("sw_alloc_delay");
  if (!_sw_alloc_delay)
  {
    Error("Switch allocator cannot have zero delay.");
  }

  vector<int> watch_flits = config.GetIntArray("watch_flits");
  for (size_t i = 0; i < watch_flits.size(); ++i)
  {
      _flits_to_watch.insert(watch_flits[i]);
  }

  vector<int> watch_packets = config.GetIntArray("watch_packets");
  for (size_t i = 0; i < watch_packets.size(); ++i)
  {
      _packets_to_watch.insert(watch_packets[i]);
  }
  vector<int> watch_routers = config.GetIntArray("watch_routers");
  for (size_t i = 0; i < watch_routers.size(); ++i)
  {
      _routers_to_watch.insert(watch_routers[i]);
  }
  // Routing
  string const rf = config.GetStr("routing_function") + "_" + config.GetStr("topology");
  map<string, tRoutingFunction>::const_iterator rf_iter = gRoutingFunctionMap.find(rf);
  if (rf_iter == gRoutingFunctionMap.end())
  {
    Error("Invalid routing function: " + rf);
  }
  _rf = rf_iter->second;

  // Alloc VC's
  _buf.resize(_inputs);
  for (int i = 0; i < _inputs; ++i)
  {
    ostringstream module_name;
    module_name << " buf_" << i;
   
    _buf[i] = new Buffer(config, _outputs, this, module_name.str(),0);
    
    module_name.str("");
  }

  // Alloc next VCs' buffer state
  _next_buf.resize(_outputs);
  for (int j = 0; j < _outputs; ++j)
  {
    ostringstream module_name;
    module_name << "next_vc_o" << j;
    /*
    if(j == 5){
      // cout<<"Router number "<<GetID()<<endl;
      _next_buf[j] = new BufferState(config, this, module_name.str(),true);
    }
    else*/
      _next_buf[j] = new BufferState(config, this, module_name.str(),false);
    module_name.str("");
  }

  // Alloc allocators
  string vc_alloc_type = config.GetStr("vc_allocator");
  if (vc_alloc_type == "piggyback")
  {
    if (!_speculative)
    {
      Error("Piggyback VC allocation requires speculative switch allocation to be enabled.");
    }
    _vc_allocator = NULL;
    _vc_rr_offset.resize(_outputs * _classes, -1);
  }
  else
  {
    _vc_allocator = Allocator::NewAllocator(this, "vc_allocator",
                                            vc_alloc_type,
                                            _vcs * _inputs,
                                            _vcs * _outputs);

    if (!_vc_allocator)
    {
      Error("Unknown vc_allocator type: " + vc_alloc_type);
    }
  }

  string sw_alloc_type = config.GetStr("sw_allocator");
  _sw_allocator = Allocator::NewAllocator(this, "sw_allocator",
                                          sw_alloc_type,
                                          _inputs * _input_speedup,
                                          _outputs * _output_speedup);

  if (!_sw_allocator)
  {
    Error("Unknown sw_allocator type: " + sw_alloc_type);
  }

  string spec_sw_alloc_type = config.GetStr("spec_sw_allocator");
  if (_speculative && (spec_sw_alloc_type != "prio"))
  {
    _spec_sw_allocator = Allocator::NewAllocator(this, "spec_sw_allocator",
                                                 spec_sw_alloc_type,
                                                 _inputs * _input_speedup,
                                                 _outputs * _output_speedup);
    if (!_spec_sw_allocator)
    {
      Error("Unknown spec_sw_allocator type: " + spec_sw_alloc_type);
    }
  }
  else
  {
    _spec_sw_allocator = NULL;
  }

  _sw_rr_offset.resize(_inputs * _input_speedup);
  for (int i = 0; i < _inputs * _input_speedup; ++i)
    _sw_rr_offset[i] = i % _input_speedup;

  _noq = config.GetInt("noq") > 0;
  if (_noq)
  {
    if (_routing_delay)
    {
      Error("NOQ requires lookahead routing to be enabled.");
    }
    if (_vcs < _outputs)
    {
      Error("NOQ requires at least as many VCs as router outputs.");
    }
  }
  _noq_next_output_port.resize(_inputs, vector<int>(_vcs, -1));
  _noq_next_vc_start.resize(_inputs, vector<int>(_vcs, -1));
  _noq_next_vc_end.resize(_inputs, vector<int>(_vcs, -1));

  // Output queues
  _output_buffer_size = config.GetInt("output_buffer_size");
  _output_buffer.resize(_outputs);
  _credit_buffer.resize(_inputs);

  // Switch configuration (when held for multiple cycles)
  _hold_switch_for_packet = (config.GetInt("hold_switch_for_packet") > 0);
  _switch_hold_in.resize(_inputs * _input_speedup, -1);
  _switch_hold_out.resize(_outputs * _output_speedup, -1);
  _switch_hold_vc.resize(_inputs * _input_speedup, -1);

  _bufferMonitor = new BufferMonitor(inputs, _classes);
  _switchMonitor = new SwitchMonitor(inputs, outputs, _classes);

#ifdef TRACK_FLOWS
  for (int c = 0; c < _classes; ++c)
  {
    _stored_flits[c].resize(_inputs, 0);
    _active_packets[c].resize(_inputs, 0);
  }
  _outstanding_classes.resize(_outputs, vector<queue<int> >(_vcs));
#endif
}

IQRouter::~IQRouter()
{

  if (gPrintActivity)
  {
    cout << Name() << ".bufferMonitor:" << endl;
    cout << *_bufferMonitor << endl;

    cout << Name() << ".switchMonitor:" << endl;
    cout << "Inputs=" << _inputs;
    cout << "Outputs=" << _outputs;
    cout << *_switchMonitor << endl;
  }

  for (int i = 0; i < _inputs; ++i)
    delete _buf[i];

  for (int j = 0; j < _outputs; ++j)
    delete _next_buf[j];

  delete _vc_allocator;
  delete _sw_allocator;
  if (_spec_sw_allocator)
    delete _spec_sw_allocator;

  delete _bufferMonitor;
  delete _switchMonitor;
}

void IQRouter::AddOutputChannel(FlitChannel *channel, CreditChannel *backchannel)
{
  int alloc_delay = _speculative ? max(_vc_alloc_delay, _sw_alloc_delay) : (_vc_alloc_delay + _sw_alloc_delay);
  int min_latency = 1 + _crossbar_delay + channel->GetLatency() + _routing_delay + alloc_delay + backchannel->GetLatency() + _credit_delay;
  // cout<<"SIZE "<<_output_channels.size()<<endl;
  _next_buf[_output_channels.size()]->SetMinLatency(min_latency);
  Router::AddOutputChannel(channel, backchannel);
}

void IQRouter::ReadInputs()
{
  bool have_flits = _ReceiveFlits();
  bool have_credits = _ReceiveCredits();
  _active = _active || have_flits || have_credits;
}



void IQRouter::_InternalStep()
{
    if (!_active)
    {
        return;
    }

    _InputQueuing();
    bool activity = !_proc_credits.empty();


    activity = _Multi_Internal_Sub_Step(activity);

    activity = _Internal_Sub_Step(activity);


    _active = activity;

    _OutputQueuing();

    _bufferMonitor->cycle();
    _switchMonitor->cycle();
}

bool IQRouter::_Internal_Sub_Step(bool activity)
{
    if (!_route_vcs.empty())
        _RouteEvaluate();

    if (_vc_allocator)
    {
        _vc_allocator->Clear();
        if (!_vc_alloc_vcs.empty())
            _VCAllocEvaluate();
    }

    if (_hold_switch_for_packet)
    {
        if (!_sw_hold_vcs.empty())
            _SWHoldEvaluate();
    }

    _sw_allocator->Clear();

    if (_spec_sw_allocator)
        _spec_sw_allocator->Clear();

    if (!_sw_alloc_vcs.empty())
        _SWAllocEvaluate();

    if (!_crossbar_flits.empty())
        _SwitchEvaluate();

    if (!_route_vcs.empty())
    {
        _RouteUpdate();
        activity = activity || !_route_vcs.empty();
    }

    if (!_vc_alloc_vcs.empty())
    {
        _VCAllocUpdate();
        activity = activity || !_vc_alloc_vcs.empty();
    }

    if (_hold_switch_for_packet)
    {
        if (!_sw_hold_vcs.empty())
        {
            _SWHoldUpdate();
            activity = activity || !_sw_hold_vcs.empty();
        }
    }

    if (!_sw_alloc_vcs.empty())
    {
        _SWAllocUpdate();
        activity = activity || !_sw_alloc_vcs.empty();
    }

    if (!_crossbar_flits.empty())
    {
        _SwitchUpdate();
        activity = activity || !_crossbar_flits.empty();
    }

    return activity;
}


bool IQRouter::_Multi_Internal_Sub_Step(bool activity)
{
    if (!_route_vcs_multi.empty())
        _RouteEvaluateMulti();

    if (_vc_allocator)
    {
        _vc_allocator->Clear();
        if (!_vc_alloc_vcs_multi.empty())
            _VCAllocEvaluateMulti();
    }

    _sw_allocator->Clear();

    if (!_sw_alloc_vcs_multi.empty())
        _SWAllocEvaluateMulti();

    // if (!_crossbar_flits_multi.empty())
    //   _SwitchEvaluateMulti();

    if (!_route_vcs_multi.empty())
    {
        _RouteUpdateMulti();
        activity = activity || !_route_vcs_multi.empty();
    }

    if (!_vc_alloc_vcs_multi.empty())
    {
        _VCAllocUpdateMulti();
        activity = activity || !_vc_alloc_vcs_multi.empty();
    }

    if (!_sw_alloc_vcs_multi.empty())
    {
        _SWAllocUpdateMulti();
        activity = activity || !_sw_alloc_vcs_multi.empty();
    }

    // if (!_crossbar_flits_multi.empty())
    // {
    //   _SwitchUpdateMulti();
    //   activity = activity || !_crossbar_flits_multi.empty();
    // }

    return activity;
}
void IQRouter::WriteOutputs()
{
  _SendFlits();
  _SendCredits();
}

//------------------------------------------------------------------------------
// read inputs
//------------------------------------------------------------------------------

bool IQRouter::_ReceiveFlits()
{
  bool activity = false;
  for (int input = 0; input < _inputs; ++input)
  {
    Flit *const f = _input_channels[input]->Receive();
    if (f)
    {

#ifdef TRACK_FLOWS
      ++_received_flits[f->cl][input];
#endif

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " Received flit " << f->id
                   << " from channel at input " << input << " vc "<<f->vc
                   << "." << endl;
      }
      _in_queue_flits.insert(make_pair(input, f));
      activity = true;
    }
  }
  return activity;
}

bool IQRouter::_ReceiveCredits()
{
  bool activity = false;
  for (int output = 0; output < _outputs; ++output)
  {
    Credit *const c = _output_credits[output]->Receive();
    if (c)
    {
      _proc_credits.push_back(make_pair(GetSimTime() + _credit_delay,
                                        make_pair(c, output)));
      activity = true;
    }
  }
  return activity;
}

//------------------------------------------------------------------------------
// input queuing
//------------------------------------------------------------------------------

void IQRouter::_InputQueuing()
{

  for (map<int, Flit *>::const_iterator iter = _in_queue_flits.begin();
       iter != _in_queue_flits.end();
       ++iter)
  {
    int const input = iter->first;
    assert((input >= 0) && (input < _inputs));

    Flit *const f = iter->second;
    assert(f);

    int const vc = f->vc;
    assert((vc >= 0) && (vc < _vcs));

    Buffer *const cur_buf = _buf[input];
    f->cur_router = GetID();
    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID()   
                 << " adding flit " << f->id 
                 <<" to input " << input << " VC " << vc
                 << " (state: " << VC::VCSTATE[cur_buf->GetState(vc)];
      if (cur_buf->Empty(vc))
      {
        *gWatchOut << ", empty";
      }
      else
      {
        assert(cur_buf->FrontFlit(vc));
        *gWatchOut << ", front: " << cur_buf->FrontFlit(vc)->id;
      }
      *gWatchOut << "). Here is input_queuing" << endl;
      
    }
    cur_buf->AddFlit(vc, f);
    

#ifdef TRACK_FLOWS
    ++_stored_flits[f->cl][input];
    if (f->head)
      ++_active_packets[f->cl][input];
#endif

    _bufferMonitor->write(input, f);


    if (cur_buf->GetState(vc) == VC::idle)
    {
      assert(cur_buf->FrontFlit(vc) == f);
      
      assert(cur_buf->GetOccupancy(vc) == 1);
      // cout<<"fid "<<f->id<<endl;
      assert(f->head);
      assert(_switch_hold_vc[input * _input_speedup + vc % _input_speedup] != vc);
      if (_routing_delay)
      {
        cur_buf->SetState(vc, VC::routing);
        if(f->mflag)
        {
          // if(GetID() == 41)
          //   cout<<"fid "<<f->id<<endl;
          _route_vcs_multi.push_back(make_pair(-1, make_pair(input, vc)));
        }
        else
        {
          _route_vcs.push_back(make_pair(-1, make_pair(input, vc)));
        }
        
      }
      else
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "Using precomputed lookahead routing information for VC " << vc
                     << " at input " << input
                     << " (front: " << f->id
                     << "). Here is input_queuing" << endl;
        }

        cur_buf->SetRouteSet(vc, &f->la_route_set);
        cur_buf->SetState(vc, VC::vc_alloc);
        if (_speculative)
        {
          _sw_alloc_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                                          -1)));
        }
        if (_vc_allocator)
        {
          _vc_alloc_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                                          -1)));
        }
        if (_noq)
        {
          _UpdateNOQ(input, vc, f);
        }
      }
    }
    //Flits other than head are added here directly to switch alloc vcs
    else if ((cur_buf->GetState(vc) == VC::active) &&
             (cur_buf->FrontFlit(vc) == f))
    {
      if (_switch_hold_vc[input * _input_speedup + vc % _input_speedup] == vc)
      {
        _sw_hold_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                                       -1)));
      }//todo add mcast
      else
      {
        if (f->mflag)
        {
          vector<int> Outpair = cur_buf->GetMulticastOutpair(vc);
          for(int i = 0; i < Outpair.size(); i++)
          {
            int output_and_vc = Outpair[i];
            _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(make_pair(make_pair(input, vc), output_and_vc),        
                                                          -1)));
          }
          if (f->watch || (_routers_to_watch.count(GetID()) > 0))
          {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                  << " push into mcast switch alloc in input qeueing, fid = " << f->id << " output pair size is "
                  << Outpair.size()<<" mcast switch to do is "
                  << _sw_alloc_vcs_multi.size()<< endl;
          }

        }
        else{
          _sw_alloc_vcs.push_back(make_pair(-1, make_pair(make_pair(input, vc),
                                                        -1)));
          if (f->watch || (_routers_to_watch.count(GetID()) > 0))
          {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                  << "push into unicast switch alloc in input qeueing, fid = " << f->id << endl;
          }

        }

      }
    }
  }
  _in_queue_flits.clear();


  while (!_proc_credits.empty())
  {

    pair<int, pair<Credit *, int> > const &item = _proc_credits.front();

    int const time = item.first;
    if (GetSimTime() < time)
    {
      break;
    }

    Credit *const c = item.second.first;
    assert(c);

    int const output = item.second.second;
    assert((output >= 0) && (output < _outputs));

    BufferState *const dest_buf = _next_buf[output];

#ifdef TRACK_FLOWS
    for (set<int>::const_iterator iter = c->vc.begin(); iter != c->vc.end(); ++iter)
    {
      int const vc = *iter;
      assert(!_outstanding_classes[output][vc].empty());
      int cl = _outstanding_classes[output][vc].front();
      _outstanding_classes[output][vc].pop();
      assert(_outstanding_credits[cl][output] > 0);
      --_outstanding_credits[cl][output];
    }
#endif
    // if(GetID() == 56 && output == 5)
      // cout<<" credit id "<<c->id<<endl;
    //   if(GetSimTime()>=999)
    // cout<<"just error iqc\n";
    dest_buf->ProcessCredit(c);
    c->Free();
    _proc_credits.pop_front();
  }
}




//------------------------------------------------------------------------------
// routing
//------------------------------------------------------------------------------

void IQRouter::_RouteEvaluate()
{
  assert(_routing_delay);

  for (deque<pair<int, pair<int, int> > >::iterator iter = _route_vcs.begin();
       iter != _route_vcs.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }
    iter->first = GetSimTime() + _routing_delay - 1;

    int const input = iter->second.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.second;
    assert((vc >= 0) && (vc < _vcs));

    Buffer const *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::routing);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " beginning routing for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }
  }
}

void IQRouter::_RouteUpdate()
{
  assert(_routing_delay);

  while (!_route_vcs.empty())
  {

    pair<int, pair<int, int> > const &item = _route_vcs.front();

    int const time = item.first;
    if ((time < 0) || (GetSimTime() < time))
    {
      break;
    }
    assert(GetSimTime() == time);

    int const input = item.second.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = item.second.second;
    assert((vc >= 0) && (vc < _vcs));

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::routing);

    Flit *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " completed routing for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }

    cur_buf->Route(vc, _rf, this, f, input);
    cur_buf->SetState(vc, VC::vc_alloc);
    if (_speculative)
    {
      _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second, -1)));
    }
    if (_vc_allocator)
    {
      _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second, -1)));
    }
    // NOTE: No need to handle NOQ here, as it requires lookahead routing!
    _route_vcs.pop_front();
  }
}

//------------------------------------------------------------------------------
// VC allocation
//------------------------------------------------------------------------------


void IQRouter::_VCAllocEvaluate()
{
  assert(_vc_allocator);

  bool watched = false;

  for (deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _vc_alloc_vcs.begin();
       iter != _vc_alloc_vcs.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }

    int const input = iter->second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    assert(iter->second.second == -1);

    Buffer const *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::vc_alloc);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      // cout<<"TIme is "<<time<<endl;
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " Beginning VC allocation for VC " << vc
                 << " at input " << input
                 << " (front fid = : " << f->id << " pid = "<<f->pid << " mflag = "<< f->mflag
                 << ")." << endl;
    }

    OutputSet const *const route_set = cur_buf->GetRouteSet(vc);
    assert(route_set);

    int const out_priority = cur_buf->GetPriority(vc);
    set<OutputSet::sSetElement> const setlist = route_set->GetSet();

    bool elig = false;
    bool cred = false;
    bool reserved = false;

    assert(!_noq || (setlist.size() == 1));

    for (set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
         iset != setlist.end();
         ++iset)
    {

      int const out_port = iset->output_port;
      assert((out_port >= 0) && (out_port < _outputs));

      BufferState const *const dest_buf = _next_buf[out_port];

      int vc_start;
      int vc_end;

      if (_noq && _noq_next_output_port[input][vc] >= 0)
      {
        assert(!_routing_delay);
        vc_start = _noq_next_vc_start[input][vc];
        vc_end = _noq_next_vc_end[input][vc];
      }
      else
      {
        vc_start = iset->vc_start;
        vc_end = iset->vc_end;
      }
      assert(vc_start >= 0 && vc_start < _vcs);
      assert(vc_end >= 0 && vc_end < _vcs);
      assert(vc_end >= vc_start);

      for (int out_vc = vc_start; out_vc <= vc_end; ++out_vc)
      {
        assert((out_vc >= 0) && (out_vc < _vcs));

        int in_priority = iset->pri;
        if (_vc_prioritize_empty && !dest_buf->IsEmptyFor(out_vc))
        {
          assert(in_priority >= 0);
          in_priority += numeric_limits<int>::min();
        }

        // On the input input side, a VC might request several output VCs.
        // These VCs can be prioritized by the routing function, and this is
        // reflected in "in_priority". On the output side, if multiple VCs are
        // requesting the same output VC, the priority of VCs is based on the
        // actual packet priorities, which is reflected in "out_priority".

        if (!dest_buf->IsAvailableFor(out_vc))
        {
          if (f->watch || (_routers_to_watch.count(GetID()) > 0))
          {
            int const use_input_and_vc = dest_buf->UsedBy(out_vc);
            int const use_input = use_input_and_vc / _vcs;
            int const use_vc = use_input_and_vc % _vcs;
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                       << "  VC " << out_vc
                       << " at output " << out_port
                       << " is in use by VC " << use_vc
                       << " at input " << use_input;
            Flit *cf = _buf[use_input]->FrontFlit(use_vc);
            *gWatchOut << " state is " << _buf[use_input]->GetState(use_vc);
            if (cf)
            {
                *gWatchOut << " (front flit: " << cf->id << " mcast = " << cf->mflag << " head = " << cf->head << " tail = " << cf->tail << ")";
            }
            else
            {
              *gWatchOut << " (empty)";
            }
            *gWatchOut << "." << endl;

          }
        }
        else
        {
          elig = true;
          if (_vc_busy_when_full && dest_buf->IsFullFor(out_vc))
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "  VC " << out_vc
                         << " at output " << out_port
                         << " is full." << " last pid = "<<dest_buf->_last_pid[out_vc] << " last fid = " << dest_buf->_last_id[out_vc] << endl;
            dest_buf->Display(*gWatchOut);
            reserved |= !dest_buf->IsFull();
          }
          else
          {
            cred = true;
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "  Requesting VC " << out_vc
                         << " at output " << out_port
                         << " (in_pri: " << in_priority
                         << ", out_pri: " << out_priority
                         << ")." << endl;
              watched = true;
            }
            int const input_and_vc = _vc_shuffle_requests ? (vc * _inputs + input) : (input * _vcs + vc);
            _vc_allocator->AddRequest(input_and_vc, out_port * _vcs + out_vc,
                                      0, in_priority, out_priority);
          }
        }
      }
    }
    if (!elig)
    {
      iter->second.second = STALL_BUFFER_BUSY;
    }
    else if (_vc_busy_when_full && !cred)
    {
      iter->second.second = reserved ? STALL_BUFFER_RESERVED : STALL_BUFFER_FULL;
    }
  }

  if (watched && (_routers_to_watch.count(GetID()) > 0))
  {
    *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | " << " rid = " <<GetID() ;
    _vc_allocator->PrintRequests(gWatchOut);
  }

  _vc_allocator->Allocate();

  if (watched && (_routers_to_watch.count(GetID()) > 0))
  {
    *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | " << " rid = " <<GetID() ;
    _vc_allocator->PrintGrants(gWatchOut);
  }

  for (deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _vc_alloc_vcs.begin();
       iter != _vc_alloc_vcs.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }
    iter->first = GetSimTime() + _vc_alloc_delay - 1;

    int const input = iter->second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    if (iter->second.second < -1)
    {
      continue;
    }

    assert(iter->second.second == -1);

    Buffer const *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::vc_alloc);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    int const input_and_vc = _vc_shuffle_requests ? (vc * _inputs + input) : (input * _vcs + vc);
    int const output_and_vc = _vc_allocator->OutputAssigned(input_and_vc);

    if (output_and_vc >= 0)
    {

      int const match_output = output_and_vc / _vcs;
      assert((match_output >= 0) && (match_output < _outputs));
      int const match_vc = output_and_vc % _vcs;
      assert((match_vc >= 0) && (match_vc < _vcs));

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " Assigning VC " << match_vc
                   << " at output " << match_output
                   << " to VC " << vc
                   << " at input " << input <<" (front fid = "<< f->id << " pid = " <<f->pid
                   << "." << endl;
      }

      iter->second.second = output_and_vc;
    }
    else
    {

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " VC allocation failed for VC " << vc
                   << " at input " << input << " (front fid = " << f->id << " pid = " << f->pid
                   << "." << endl;
      }

      iter->second.second = STALL_BUFFER_CONFLICT;
    }
  }

  if (_vc_alloc_delay <= 1)
  {
    return;
  }

  for (deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _vc_alloc_vcs.begin();
       iter != _vc_alloc_vcs.end();
       ++iter)
  {

    int const time = iter->first;
    assert(time >= 0);
    if (GetSimTime() < time)
    {
      break;
    }

    assert(iter->second.second != -1);

    int const output_and_vc = iter->second.second;

    if (output_and_vc >= 0)
    {

      int const match_output = output_and_vc / _vcs;
      assert((match_output >= 0) && (match_output < _outputs));
      int const match_vc = output_and_vc % _vcs;
      assert((match_vc >= 0) && (match_vc < _vcs));

      BufferState const *const dest_buf = _next_buf[match_output];

      int const input = iter->second.first.first;
      assert((input >= 0) && (input < _inputs));
      int const vc = iter->second.first.second;
      assert((vc >= 0) && (vc < _vcs));

      Buffer const *const cur_buf = _buf[input];
      assert(!cur_buf->Empty(vc));
      assert(cur_buf->GetState(vc) == VC::vc_alloc);

      Flit const *const f = cur_buf->FrontFlit(vc);
      assert(f);
      assert(f->vc == vc);
      assert(f->head);

      if (!dest_buf->IsAvailableFor(match_vc))
      {
        if (f->watch)
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << " Discarding previously generated grant for VC " << vc
                     << " at input " << input
                     << ": VC " << match_vc
                     << " at output " << match_output
                     << " is no longer available." << endl;
        }
        iter->second.second = STALL_BUFFER_BUSY;
      }
      else if (_vc_busy_when_full && dest_buf->IsFullFor(match_vc))
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << " Discarding previously generated grant for VC " << vc
                     << " at input " << input
                     << ": VC " << match_vc
                     << " at output " << match_output
                     << " has become full." << endl;
        }
        iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
      }
    }
  }
}

void IQRouter::_VCAllocUpdate()
{
  assert(_vc_allocator);

  while (!_vc_alloc_vcs.empty())
  {

    pair<int, pair<pair<int, int>, int> > const &item = _vc_alloc_vcs.front();

    int const time = item.first;
    if ((time < 0) || (GetSimTime() < time))
    {
      break;
    }
    assert(GetSimTime() == time);

    int const input = item.second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = item.second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    assert(item.second.second != -1);

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::vc_alloc);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " Completed VC allocation for VC " << vc
                 << " at input " << input
                 << " (front fid= " << f->id << " pid= " << f->pid
                 << ")." << endl;
    }

    int const output_and_vc = item.second.second;

    if (output_and_vc >= 0)
    {

      int const match_output = output_and_vc / _vcs;
      assert((match_output >= 0) && (match_output < _outputs));
      int const match_vc = output_and_vc % _vcs;
      assert((match_vc >= 0) && (match_vc < _vcs));

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Acquiring assigned VC " << match_vc
                   << " at output " << match_output << " for fid= " << f->id << " pid= " << f->pid
                   << "." << endl;
      }

      BufferState *const dest_buf = _next_buf[match_output];
      assert(dest_buf->IsAvailableFor(match_vc));

      dest_buf->TakeBuffer(match_vc, input * _vcs + vc);

      cur_buf->SetOutput(vc, match_output, match_vc);
      cur_buf->SetState(vc, VC::active);
      if (!_speculative)
      {
        _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first, -1)));
      }
    }
    else
    {
      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  No output VC allocated." << endl;
      }

#ifdef TRACK_STALLS
      assert((output_and_vc == STALL_BUFFER_BUSY) ||
             (output_and_vc == STALL_BUFFER_CONFLICT));
      if (output_and_vc == STALL_BUFFER_BUSY)
      {
        ++_buffer_busy_stalls[f->cl];
      }
      else if (output_and_vc == STALL_BUFFER_CONFLICT)
      {
        ++_buffer_conflict_stalls[f->cl];
      }
#endif

      _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first, -1)));
    }
    _vc_alloc_vcs.pop_front();
  }
}


//------------------------------------------------------------------------------
// switch holding
//------------------------------------------------------------------------------

void IQRouter::_SWHoldEvaluate()
{
  assert(_hold_switch_for_packet);

  for (deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_hold_vcs.begin();
       iter != _sw_hold_vcs.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }
    iter->first = GetSimTime();

    int const input = iter->second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    assert(iter->second.second == -1);

    Buffer const *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::active);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " beginning held switch allocation for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }

    int const expanded_input = input * _input_speedup + vc % _input_speedup;
    assert(_switch_hold_vc[expanded_input] == vc);

    int const match_port = cur_buf->GetOutputPort(vc);
    assert((match_port >= 0) && (match_port < _outputs));
    int const match_vc = cur_buf->GetOutputVC(vc);
    assert((match_vc >= 0) && (match_vc < _vcs));

    int const expanded_output = match_port * _output_speedup + input % _output_speedup;
    assert(_switch_hold_in[expanded_input] == expanded_output);

    BufferState const *const dest_buf = _next_buf[match_port];

    if (dest_buf->IsFullFor(match_vc))
    {
      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Unable to reuse held connection from input " << input
                   << "." << (expanded_input % _input_speedup)
                   << " to output " << match_port
                   << "." << (expanded_output % _output_speedup)
                   << ": No credit available." << endl;
      }
      iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
    }
    else
    {
      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Reusing held connection from input " << input
                   << "." << (expanded_input % _input_speedup)
                   << " to output " << match_port
                   << "." << (expanded_output % _output_speedup)
                   << "." << endl;
      }
      iter->second.second = expanded_output;
    }
  }
}

void IQRouter::_SWHoldUpdate()
{
  assert(_hold_switch_for_packet);

  while (!_sw_hold_vcs.empty())
  {

    pair<int, pair<pair<int, int>, int> > const &item = _sw_hold_vcs.front();

    int const time = item.first;
    if (time < 0)
    {
      break;
    }
    assert(GetSimTime() == time);

    int const input = item.second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = item.second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    assert(item.second.second != -1);

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::active);

    Flit *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " completed held switch allocation for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }

    int const expanded_input = input * _input_speedup + vc % _input_speedup;
    assert(_switch_hold_vc[expanded_input] == vc);

    int const expanded_output = item.second.second;

    if (expanded_output >= 0 && (_output_buffer_size == -1 || _output_buffer[expanded_output / _output_speedup].size() < size_t(_output_buffer_size)))
    {

      assert(_switch_hold_in[expanded_input] == expanded_output);
      assert(_switch_hold_out[expanded_output] == expanded_input);

      int const output = expanded_output / _output_speedup;
      assert((output >= 0) && (output < _outputs));
      assert(cur_buf->GetOutputPort(vc) == output);

      int const match_vc = cur_buf->GetOutputVC(vc);
      assert((match_vc >= 0) && (match_vc < _vcs));

      BufferState *const dest_buf = _next_buf[output];

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Scheduling switch connection from input " << input
                   << "." << (vc % _input_speedup)
                   << " to output " << output
                   << "." << (expanded_output % _output_speedup)
                   << "." << endl;
      }

      cur_buf->RemoveFlit(vc);

#ifdef TRACK_FLOWS
      --_stored_flits[f->cl][input];
      if (f->tail)
        --_active_packets[f->cl][input];
#endif

      _bufferMonitor->read(input, f);

      f->hops++;
      f->vc = match_vc;

      if (!_routing_delay && f->head)
      {
        const FlitChannel *channel = _output_channels[output];
        const Router *router = channel->GetSink();
        if (router)
        {
          if (_noq)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "Updating lookahead routing information for flit " << f->id
                         << " (NOQ)." << endl;
            }
            int next_output_port = _noq_next_output_port[input][vc];
            assert(next_output_port >= 0);
            _noq_next_output_port[input][vc] = -1;
            int next_vc_start = _noq_next_vc_start[input][vc];
            assert(next_vc_start >= 0 && next_vc_start < _vcs);
            _noq_next_vc_start[input][vc] = -1;
            int next_vc_end = _noq_next_vc_end[input][vc];
            assert(next_vc_end >= 0 && next_vc_end < _vcs);
            _noq_next_vc_end[input][vc] = -1;
            f->la_route_set.Clear();
            f->la_route_set.AddRange(next_output_port, next_vc_start, next_vc_end);
          }
          else
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "Updating lookahead routing information for flit " << f->id
                         << "." << endl;
            }
            int in_channel = channel->GetSinkPort();
            _rf(router, f, in_channel, &f->la_route_set, false);
          }
        }
        else
        {
          f->la_route_set.Clear();
        }
      }

#ifdef TRACK_FLOWS
      ++_outstanding_credits[f->cl][output];
      _outstanding_classes[output][f->vc].push(f->cl);
#endif

      dest_buf->SendingFlit(f);

      _crossbar_flits.push_back(make_pair(-1, make_pair(f, make_pair(expanded_input, expanded_output))));

      if (_out_queue_credits.count(input) == 0)
      {
        _out_queue_credits.insert(make_pair(input, Credit::New()));
      }
      _out_queue_credits.find(input)->second->vc.insert(vc);

      if (cur_buf->Empty(vc))
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  Cancelling held connection from input " << input
                     << "." << (expanded_input % _input_speedup)
                     << " to " << output
                     << "." << (expanded_output % _output_speedup)
                     << ": No more flits." << endl;
        }
        _switch_hold_vc[expanded_input] = -1;
        _switch_hold_in[expanded_input] = -1;
        _switch_hold_out[expanded_output] = -1;
        if (f->tail)
        {
          cur_buf->SetState(vc, VC::idle);
        }
      }
      else
      {
        Flit *const nf = cur_buf->FrontFlit(vc);
        assert(nf);
        assert(nf->vc == vc);
        if (f->tail)
        {
          assert(nf->head);
          if (f->watch || (_routers_to_watch.count(GetID()) > 0))
          {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                       << "  Cancelling held connection from input " << input
                       << "." << (expanded_input % _input_speedup)
                       << " to " << output
                       << "." << (expanded_output % _output_speedup)
                       << ": End of packet." << endl;
          }
          _switch_hold_vc[expanded_input] = -1;
          _switch_hold_in[expanded_input] = -1;
          _switch_hold_out[expanded_output] = -1;
          if (_routing_delay)
          {
            cur_buf->SetState(vc, VC::routing);
            if (nf->mflag) {
                _route_vcs_multi.push_back(make_pair(-1, item.second.first));
                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                        << "  flit " << nf->id
                        << " will be mcast-routed in switch hold update " << endl;
                }
            }
            
            else
            {
                _route_vcs.push_back(make_pair(-1, item.second.first));
                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                        << "  flit " << nf->id
                        << " will be unicast-routed in " << endl;
                }
            }
          }
          else
          {
            if (nf->watch)
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "Using precomputed lookahead routing information for VC " << vc
                         << " at input " << input
                         << " (front: " << nf->id
                         << ")." << endl;
            }
            cur_buf->SetRouteSet(vc, &nf->la_route_set);
            cur_buf->SetState(vc, VC::vc_alloc);
            if (_speculative)
            {
              _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                              -1)));
            }
            if (_vc_allocator)
            {
              _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                              -1)));
            }
            if (_noq)
            {
              _UpdateNOQ(input, vc, nf);
            }
          }
        }
        else
        {
          _sw_hold_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                         -1)));
        }
      }
    }
    else
    {
      //when internal speedup >1.0, the buffer stall stats may not be accruate
      assert((expanded_output == STALL_BUFFER_FULL) ||
             (expanded_output == STALL_BUFFER_RESERVED) || !(_output_buffer_size == -1 || _output_buffer[expanded_output / _output_speedup].size() < size_t(_output_buffer_size)));

      int const held_expanded_output = _switch_hold_in[expanded_input];
      assert(held_expanded_output >= 0);

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Cancelling held connection from input " << input
                   << "." << (expanded_input % _input_speedup)
                   << " to " << (held_expanded_output / _output_speedup)
                   << "." << (held_expanded_output % _output_speedup)
                   << ": Flit not sent." << endl;
      }
      _switch_hold_vc[expanded_input] = -1;
      _switch_hold_in[expanded_input] = -1;
      _switch_hold_out[held_expanded_output] = -1;
      _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                      -1)));
    }
    _sw_hold_vcs.pop_front();
  }
}

//------------------------------------------------------------------------------
// switch allocation
//------------------------------------------------------------------------------

bool IQRouter::_SWAllocAddReq(int input, int vc, int output)
{
  assert(input >= 0 && input < _inputs);
  assert(vc >= 0 && vc < _vcs);
  assert(output >= 0 && output < _outputs);

  // When input_speedup > 1, the virtual channel buffers are interleaved to
  // create multiple input ports to the switch. Similarily, the output ports
  // are interleaved based on their originating input when output_speedup > 1.

  int const expanded_input = input * _input_speedup + vc % _input_speedup;
  int const expanded_output = output * _output_speedup + input % _output_speedup;

  Buffer const *const cur_buf = _buf[input];
  assert(!cur_buf->Empty(vc));
  
  assert((cur_buf->GetState(vc) == VC::active) ||
         (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

  Flit const *const f = cur_buf->FrontFlit(vc);
  assert(f);
  assert(f->vc == vc);

  if ((_switch_hold_in[expanded_input] < 0) &&
      (_switch_hold_out[expanded_output] < 0))
  {

    Allocator *allocator = _sw_allocator;
    int prio = cur_buf->GetPriority(vc);

    if (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc))
    {
      if (_spec_sw_allocator)
      {
        allocator = _spec_sw_allocator;
      }
      else
      {
        assert(prio >= 0);
        prio += numeric_limits<int>::min();
      }
    }

    Allocator::sRequest req;
  //Following block is skipped for the first iter
    if (allocator->ReadRequest(req, expanded_input, expanded_output))
    {
      if (RoundRobinArbiter::Supersedes(vc, prio, req.label, req.in_pri,
                                        _sw_rr_offset[expanded_input], _vcs)) //Only body and tail
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  Replacing earlier request from VC " << req.label
                     << " for output " << output
                     << "." << (expanded_output % _output_speedup)
                     << " with priority " << req.in_pri
                     << " (" << ((cur_buf->GetState(vc) == VC::active) ? "non-spec" : "spec")
                     << ", pri: " << prio
                     << ")." << endl;
        }
        allocator->RemoveRequest(expanded_input, expanded_output, req.label);
        allocator->AddRequest(expanded_input, expanded_output, vc, prio, prio);
        return true;
      }

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Output " << output
                   << "." << (expanded_output % _output_speedup)
                   << " was already requested by VC " << req.label
                   << " with priority " << req.in_pri
                   << " (pri: " << prio
                   << ")." << endl;
      }
      return false;
    }

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << "  Requesting output " << output
                 << "." << (expanded_output % _output_speedup)
                 << " (" << ((cur_buf->GetState(vc) == VC::active) ? "non-spec" : "spec")
                 << ", pri: " << prio
                 << ")." << endl;
    }
    allocator->AddRequest(expanded_input, expanded_output, vc, prio, prio);
    return true;
  }

  if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  {
    *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
               << "  Ignoring output " << output
               << "." << (expanded_output % _output_speedup)
               << " due to switch hold (";
    if (_switch_hold_in[expanded_input] >= 0)
    {
      *gWatchOut << "input: " << input
                 << "." << (expanded_input % _input_speedup);
      if (_switch_hold_out[expanded_output] >= 0)
      {
        *gWatchOut << ", ";
      }
    }
    if (_switch_hold_out[expanded_output] >= 0)
    {
      *gWatchOut << "output: " << output
                 << "." << (expanded_output % _output_speedup);
    }
    *gWatchOut << ")." << endl;
  }
  return false;
}

void IQRouter::_SWAllocEvaluate()
{
  bool watched = false;

  for (deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_alloc_vcs.begin();
       iter != _sw_alloc_vcs.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }

    int const input = iter->second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    assert(iter->second.second == -1);

    assert(_switch_hold_vc[input * _input_speedup + vc % _input_speedup] != vc);

    Buffer const *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert((cur_buf->GetState(vc) == VC::active) ||
           (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

    Flit const *const f = cur_buf->FrontFlit(vc);

    assert(f);
    assert(f->vc == vc);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " beginning switch allocation for VC " << vc
                 << " at input " << input
                 << " (front fid: " << f->id << " pid = "<<f->pid 
                 << ")." << endl;
    }

    if (cur_buf->GetState(vc) == VC::active) //all flits go here and then just calls continue
    {

      int const dest_output = cur_buf->GetOutputPort(vc);
      assert((dest_output >= 0) && (dest_output < _outputs));
      int const dest_vc = cur_buf->GetOutputVC(vc);
      assert((dest_vc >= 0) && (dest_vc < _vcs));

      BufferState const *const dest_buf = _next_buf[dest_output];

      if (dest_buf->IsFullFor(dest_vc) || (_output_buffer_size != -1 && _output_buffer[dest_output].size() >= (size_t)(_output_buffer_size)))
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  VC " << dest_vc
                     << " at output " << dest_output
                     << " is full." << " last pid = " << dest_buf->_last_pid[dest_vc] << "last fid = " << dest_buf->_last_id[dest_vc] << endl;
          dest_buf->Display(*gWatchOut);
          *gWatchOut << GetSimTime() << " | " << dest_buf->IsFullFor(dest_vc) << " || "
                     << "  outbufsize " << _output_buffer_size
                     << " output_buffer[dest_output].size() " << _output_buffer[dest_output].size()
                     << endl;
        }
        iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
        continue;
      }
      bool const requested = _SWAllocAddReq(input, vc, dest_output);
      watched |= requested && f->watch;
      continue;
    }
    assert(_speculative && (cur_buf->GetState(vc) == VC::vc_alloc));
    assert(f->head);

    // The following models the speculative VC allocation aspects of the
    // pipeline. An input VC with a request in for an egress virtual channel
    // will also speculatively bid for the switch regardless of whether the VC
    // allocation succeeds.

    OutputSet const *const route_set = cur_buf->GetRouteSet(vc);
    assert(route_set);

    set<OutputSet::sSetElement> const setlist = route_set->GetSet();

    assert(!_noq || (setlist.size() == 1));

    for (set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
         iset != setlist.end();
         ++iset)
    {

      int const dest_output = iset->output_port;
      assert((dest_output >= 0) && (dest_output < _outputs));

      // for lower levels of speculation, ignore credit availability and always
      // issue requests for all output ports in route set

      BufferState const *const dest_buf = _next_buf[dest_output];

      bool elig = false;
      bool cred = false;

      if (_spec_check_elig)
      {

        // for higher levels of speculation, check if at least one suitable VC
        // is available at the current output

        int vc_start;
        int vc_end;

        if (_noq && _noq_next_output_port[input][vc] >= 0)
        {
          assert(!_routing_delay);
          vc_start = _noq_next_vc_start[input][vc];
          vc_end = _noq_next_vc_end[input][vc];
        }
        else
        {
          vc_start = iset->vc_start;
          vc_end = iset->vc_end;
        }
        assert(vc_start >= 0 && vc_start < _vcs);
        assert(vc_end >= 0 && vc_end < _vcs);
        assert(vc_end >= vc_start);

        for (int dest_vc = vc_start; dest_vc <= vc_end; ++dest_vc)
        {
          assert((dest_vc >= 0) && (dest_vc < _vcs));

          if (dest_buf->IsAvailableFor(dest_vc) && (_output_buffer_size == -1 || _output_buffer[dest_output].size() < (size_t)(_output_buffer_size)))
          {
            elig = true;
            if (!_spec_check_cred || !dest_buf->IsFullFor(dest_vc))
            {
              cred = true;
              break;
            }
          }
        }
      }

      if (_spec_check_elig && !elig)
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  Output " << dest_output
                     << " has no suitable VCs available." << endl;
        }
        iter->second.second = STALL_BUFFER_BUSY;
      }
      else if (_spec_check_cred && !cred)
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  All suitable VCs at output " << dest_output
                     << " are full." << endl;
        }
        iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
      }
      else
      {
        bool const requested = _SWAllocAddReq(input, vc, dest_output);
        watched |= requested && f->watch;
      }
    }
  }

  if (watched)
  {
    *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
    _sw_allocator->PrintRequests(gWatchOut);
    if (_spec_sw_allocator)
    {
      *gWatchOut << GetSimTime() << " | " << _spec_sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
      _spec_sw_allocator->PrintRequests(gWatchOut);
    }
  }

  _sw_allocator->Allocate();
  if (_spec_sw_allocator)
    _spec_sw_allocator->Allocate();

  if (watched)
  {
    *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
    _sw_allocator->PrintGrants(gWatchOut);
    if (_spec_sw_allocator)
    {
      *gWatchOut << GetSimTime() << " | " << _spec_sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
      _spec_sw_allocator->PrintGrants(gWatchOut);
    }
  }

  for (deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_alloc_vcs.begin();
       iter != _sw_alloc_vcs.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }
    iter->first = GetSimTime() + _sw_alloc_delay - 1;

    int const input = iter->second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    if (iter->second.second < -1)
    {
      continue;
    }

    assert(iter->second.second == -1);

    Buffer const *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert((cur_buf->GetState(vc) == VC::active) ||
           (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);

    int const expanded_input = input * _input_speedup + vc % _input_speedup;

    int expanded_output = _sw_allocator->OutputAssigned(expanded_input);

    if (expanded_output >= 0)
    {
      assert((expanded_output % _output_speedup) == (input % _output_speedup));
      int const granted_vc = _sw_allocator->ReadRequest(expanded_input, expanded_output);
      if (granted_vc == vc)
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << " assigning output " << (expanded_output / _output_speedup)
                     << "." << (expanded_output % _output_speedup)
                     << " to VC " << vc
                     << " at input " << input
                     << "." << (vc % _input_speedup)
                     << "." << endl;
        }
        _sw_rr_offset[expanded_input] = (vc + _input_speedup) % _vcs;
        iter->second.second = expanded_output;
      }
      else
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "Switch allocation failed for VC " << vc
                     << " at input " << input
                     << ": Granted to VC " << granted_vc << "." << endl;
        }
        iter->second.second = STALL_CROSSBAR_CONFLICT;
      }
    }
    else if (_spec_sw_allocator)
    {
      expanded_output = _spec_sw_allocator->OutputAssigned(expanded_input);
      if (expanded_output >= 0)
      {
        assert((expanded_output % _output_speedup) == (input % _output_speedup));
        if (_spec_mask_by_reqs &&
            _sw_allocator->OutputHasRequests(expanded_output))
        {
          if (f->watch || (_routers_to_watch.count(GetID()) > 0))
          {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                       << " Discarding speculative grant for VC " << vc
                       << " at input " << input
                       << "." << (vc % _input_speedup)
                       << " because output " << (expanded_output / _output_speedup)
                       << "." << (expanded_output % _output_speedup)
                       << " has non-speculative requests." << endl;
          }
          iter->second.second = STALL_CROSSBAR_CONFLICT;
        }
        else if (!_spec_mask_by_reqs &&
                 (_sw_allocator->InputAssigned(expanded_output) >= 0))
        {
          if (f->watch || (_routers_to_watch.count(GetID()) > 0))
          {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                       << " Discarding speculative grant for VC " << vc
                       << " at input " << input
                       << "." << (vc % _input_speedup)
                       << " because output " << (expanded_output / _output_speedup)
                       << "." << (expanded_output % _output_speedup)
                       << " has a non-speculative grant." << endl;
          }
          iter->second.second = STALL_CROSSBAR_CONFLICT;
        }
        else
        {
          int const granted_vc = _spec_sw_allocator->ReadRequest(expanded_input,
                                                                 expanded_output);
          if (granted_vc == vc)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << " assigning output " << (expanded_output / _output_speedup)
                         << "." << (expanded_output % _output_speedup)
                         << " to VC " << vc
                         << " at input " << input
                         << "." << (vc % _input_speedup)
                         << "." << endl;
            }
            _sw_rr_offset[expanded_input] = (vc + _input_speedup) % _vcs;
            iter->second.second = expanded_output;
          }
          else
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "Switch allocation failed for VC " << vc
                         << " at input " << input
                         << ": Granted to VC " << granted_vc << "." << endl;
            }
            iter->second.second = STALL_CROSSBAR_CONFLICT;
          }
        }
      }
      else
      {

        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "Switch allocation failed for VC " << vc
                     << " at input " << input
                     << ": No output granted." << endl;
        }

        iter->second.second = STALL_CROSSBAR_CONFLICT;
      }
    }
    else
    {

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "Switch allocation failed for VC " << vc
                   << " at input " << input
                   << ": No output granted." << endl;
      }

      iter->second.second = STALL_CROSSBAR_CONFLICT;
    }
  }

  if (!_speculative && (_sw_alloc_delay <= 1))
  {

    return;
  }

  for (deque<pair<int, pair<pair<int, int>, int> > >::iterator iter = _sw_alloc_vcs.begin();
       iter != _sw_alloc_vcs.end();
       ++iter)
  {
    int const time = iter->first;
    assert(time >= 0);
    if (GetSimTime() < time)
    {
      break;
    }

    assert(iter->second.second != -1);

    int const expanded_output = iter->second.second;

    if (expanded_output >= 0)
    {

      int const output = expanded_output / _output_speedup;
      assert((output >= 0) && (output < _outputs));

      BufferState const *const dest_buf = _next_buf[output];

      int const input = iter->second.first.first;
      assert((input >= 0) && (input < _inputs));
      assert((input % _output_speedup) == (expanded_output % _output_speedup));
      int const vc = iter->second.first.second;
      assert((vc >= 0) && (vc < _vcs));

      int const expanded_input = input * _input_speedup + vc % _input_speedup;
      assert(_switch_hold_vc[expanded_input] != vc);

      Buffer const *const cur_buf = _buf[input];
      assert(!cur_buf->Empty(vc));
      assert((cur_buf->GetState(vc) == VC::active) ||
             (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

      Flit const *const f = cur_buf->FrontFlit(vc);
      assert(f);
      assert(f->vc == vc);

      if ((_switch_hold_in[expanded_input] >= 0) ||
          (_switch_hold_out[expanded_output] >= 0))
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << " Discarding grant from input " << input
                     << "." << (vc % _input_speedup)
                     << " to output " << output
                     << "." << (expanded_output % _output_speedup)
                     << " due to conflict with held connection at ";
          if (_switch_hold_in[expanded_input] >= 0)
          {
            *gWatchOut << "input";
          }
          if ((_switch_hold_in[expanded_input] >= 0) &&
              (_switch_hold_out[expanded_output] >= 0))
          {
            *gWatchOut << " and ";
          }
          if (_switch_hold_out[expanded_output] >= 0)
          {
            *gWatchOut << "output";
          }
          *gWatchOut << "." << endl;
        }
        iter->second.second = STALL_CROSSBAR_CONFLICT;
      }
      else if (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc))
      {

        assert(f->head);

        if (_vc_allocator)
        { // separate VC and switch allocators

          int const input_and_vc =
              _vc_shuffle_requests ? (vc * _inputs + input) : (input * _vcs + vc);
          int const output_and_vc = _vc_allocator->OutputAssigned(input_and_vc);

          if (output_and_vc < 0)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << " Discarding grant from input " << input
                         << "." << (vc % _input_speedup)
                         << " to output " << output
                         << "." << (expanded_output % _output_speedup)
                         << " due to misspeculation." << endl;
            }
            iter->second.second = -1; // stall is counted in VC allocation path!
          }
          else if ((output_and_vc / _vcs) != output)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << " Discarding grant from input " << input
                         << "." << (vc % _input_speedup)
                         << " to output " << output
                         << "." << (expanded_output % _output_speedup)
                         << " due to port mismatch between VC and switch allocator." << endl;
            }
            iter->second.second = STALL_BUFFER_CONFLICT; // count this case as if we had failed allocation
          }
          else if (dest_buf->IsFullFor((output_and_vc % _vcs)))
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << " Discarding grant from input " << input
                         << "." << (vc % _input_speedup)
                         << " to output " << output
                         << "." << (expanded_output % _output_speedup)
                         << " due to lack of credit." << endl;
            }
            iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
          }
        }
        else
        { // VC allocation is piggybacked onto switch allocation

          OutputSet const *const route_set = cur_buf->GetRouteSet(vc);
          assert(route_set);

          set<OutputSet::sSetElement> const setlist = route_set->GetSet();

          bool busy = true;
          bool full = true;
          bool reserved = false;

          assert(!_noq || (setlist.size() == 1));

          for (set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
               iset != setlist.end();
               ++iset)
          {
            if (iset->output_port == output)
            {

              int vc_start;
              int vc_end;

              if (_noq && _noq_next_output_port[input][vc] >= 0)
              {
                assert(!_routing_delay);
                vc_start = _noq_next_vc_start[input][vc];
                vc_end = _noq_next_vc_end[input][vc];
              }
              else
              {
                vc_start = iset->vc_start;
                vc_end = iset->vc_end;
              }
              assert(vc_start >= 0 && vc_start < _vcs);
              assert(vc_end >= 0 && vc_end < _vcs);
              assert(vc_end >= vc_start);

              for (int out_vc = vc_start; out_vc <= vc_end; ++out_vc)
              {
                assert((out_vc >= 0) && (out_vc < _vcs));
                if (dest_buf->IsAvailableFor(out_vc))
                {
                  busy = false;
                  if (!dest_buf->IsFullFor(out_vc))
                  {
                    full = false;
                    break;
                  }
                  else if (!dest_buf->IsFull())
                  {
                    reserved = true;
                  }
                }
              }
              if (!full)
              {
                break;
              }
            }
          }

          if (busy)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << " Discarding grant from input " << input
                         << "." << (vc % _input_speedup)
                         << " to output " << output
                         << "." << (expanded_output % _output_speedup)
                         << " because no suitable output VC for piggyback allocation is available." << endl;
            }
            iter->second.second = STALL_BUFFER_BUSY;
          }
          else if (full)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << " Discarding grant from input " << input
                         << "." << (vc % _input_speedup)
                         << " to output " << output
                         << "." << (expanded_output % _output_speedup)
                         << " because all suitable output VCs for piggyback allocation are full." << endl;
            }
            iter->second.second = reserved ? STALL_BUFFER_RESERVED : STALL_BUFFER_FULL;
          }
        }
      }
      else
      {
        assert(cur_buf->GetOutputPort(vc) == output);

        int const match_vc = cur_buf->GetOutputVC(vc);
        assert((match_vc >= 0) && (match_vc < _vcs));

        if (dest_buf->IsFullFor(match_vc))
        {
          if (f->watch || (_routers_to_watch.count(GetID()) > 0))
          {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                       << "  Discarding grant from input " << input
                       << "." << (vc % _input_speedup)
                       << " to output " << output
                       << "." << (expanded_output % _output_speedup)
                       << " due to lack of credit." << endl;
          }
          iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
        }
      }
    }
  }
}

void IQRouter::_SWAllocUpdate()
{
  while (!_sw_alloc_vcs.empty())
  {

    pair<int, pair<pair<int, int>, int> > const &item = _sw_alloc_vcs.front();

    int const time = item.first;
    if ((time < 0) || (GetSimTime() < time))
    {
      break;
    }
    assert(GetSimTime() == time);

    int const input = item.second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = item.second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert((cur_buf->GetState(vc) == VC::active) ||
           (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

    Flit *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " completed switch allocation for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }

    int const expanded_output = item.second.second;

    if (expanded_output >= 0)
    {

      int const expanded_input = input * _input_speedup + vc % _input_speedup;
      assert(_switch_hold_vc[expanded_input] < 0);
      assert(_switch_hold_in[expanded_input] < 0);
      assert(_switch_hold_out[expanded_output] < 0);

      int const output = expanded_output / _output_speedup;
      assert((output >= 0) && (output < _outputs));

      BufferState *const dest_buf = _next_buf[output];

      int match_vc;

      if (!_vc_allocator && (cur_buf->GetState(vc) == VC::vc_alloc))
      {
        
        assert(f->head);

        int const cl = f->cl;
        assert((cl >= 0) && (cl < _classes));

        int const vc_offset = _vc_rr_offset[output * _classes + cl];

        match_vc = -1;
        int match_prio = numeric_limits<int>::min();

        const OutputSet *route_set = cur_buf->GetRouteSet(vc);
        set<OutputSet::sSetElement> const setlist = route_set->GetSet();

        assert(!_noq || (setlist.size() == 1));

        for (set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
             iset != setlist.end();
             ++iset)
        {
          if (iset->output_port == output)
          {

            int vc_start;
            int vc_end;

            if (_noq && _noq_next_output_port[input][vc] >= 0)
            {
              assert(!_routing_delay);
              vc_start = _noq_next_vc_start[input][vc];
              vc_end = _noq_next_vc_end[input][vc];
            }
            else
            {
              vc_start = iset->vc_start;
              vc_end = iset->vc_end;
            }
            assert(vc_start >= 0 && vc_start < _vcs);
            assert(vc_end >= 0 && vc_end < _vcs);
            assert(vc_end >= vc_start);

            for (int out_vc = vc_start; out_vc <= vc_end; ++out_vc)
            {
              assert((out_vc >= 0) && (out_vc < _vcs));

              int vc_prio = iset->pri;
              if (_vc_prioritize_empty && !dest_buf->IsEmptyFor(out_vc))
              {
                assert(vc_prio >= 0);
                vc_prio += numeric_limits<int>::min();
              }

              // FIXME: This check should probably be performed in Evaluate(),
              // not Update(), as the latter can cause the outcome to depend on
              // the order of evaluation!
              if (dest_buf->IsAvailableFor(out_vc) &&
                  !dest_buf->IsFullFor(out_vc) &&
                  ((match_vc < 0) ||
                   RoundRobinArbiter::Supersedes(out_vc, vc_prio,
                                                 match_vc, match_prio,
                                                 vc_offset, _vcs)))
              {
                match_vc = out_vc;
                match_prio = vc_prio;
              }
            }
          }
        }
        assert(match_vc >= 0);

        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  Allocating VC " << match_vc
                     << " at output " << output
                     << " via piggyback VC allocation." << endl;
        }

        cur_buf->SetState(vc, VC::active);
        cur_buf->SetOutput(vc, output, match_vc);
        dest_buf->TakeBuffer(match_vc, input * _vcs + vc);

        _vc_rr_offset[output * _classes + cl] = (match_vc + 1) % _vcs;
      }
      else
      {

        assert(cur_buf->GetOutputPort(vc) == output);

        match_vc = cur_buf->GetOutputVC(vc);
      }
      assert((match_vc >= 0) && (match_vc < _vcs));

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Scheduling switch connection from input " << input
                   << "." << (vc % _input_speedup)
                   << " to output " << output
                   << "." << (expanded_output % _output_speedup)
                   << "." << endl;
      }

      cur_buf->RemoveFlit(vc);
      

#ifdef TRACK_FLOWS
      --_stored_flits[f->cl][input];
      if (f->tail)
        --_active_packets[f->cl][input];
#endif

      _bufferMonitor->read(input, f);

      f->hops++;
      f->vc = match_vc;
      /*
      if(cur_buf->GetOutputPort(vc) == 5) //5 is the port to hub
      {
        //Bransan Statistacks
        int rid = hub_mapper[GetID()].second;

        if(f->head)
        {
          wait_clock[rid].insert(make_pair(f->pid,GetSimTime()));
          _cur_inter_hub = hub_mapper[f->dest].second;  
          cur_buf->SetInterDest(vc, _cur_inter_hub);
        }
        f->inter_dest = cur_buf->GetInterDest(vc);
        
        if(f->tail)
        {
          
          assert(wait_clock[rid].find(f->pid) != wait_clock[rid].end());
          time_and_cnt[rid].first += (GetSimTime() - wait_clock[rid][f->pid]);
          time_and_cnt[rid].second++;
          wait_clock[rid].erase(f->pid);

          cur_buf->SetInterDest(vc, -1);
        }
      }*/
      if (!_routing_delay && f->head)
      {
        const FlitChannel *channel = _output_channels[output];
        const Router *router = channel->GetSink();
        if (router)
        {
          if (_noq)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << " Updating lookahead routing information for flit " << f->id
                         << " (NOQ)." << endl;
            }
            int next_output_port = _noq_next_output_port[input][vc];
            assert(next_output_port >= 0);
            _noq_next_output_port[input][vc] = -1;
            int next_vc_start = _noq_next_vc_start[input][vc];
            assert(next_vc_start >= 0 && next_vc_start < _vcs);
            _noq_next_vc_start[input][vc] = -1;
            int next_vc_end = _noq_next_vc_end[input][vc];
            assert(next_vc_end >= 0 && next_vc_end < _vcs);
            _noq_next_vc_end[input][vc] = -1;
            f->la_route_set.Clear();
            f->la_route_set.AddRange(next_output_port, next_vc_start, next_vc_end);
          }
          else
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "Updating lookahead routing information for flit " << f->id
                         << "." << endl;
            }
            int in_channel = channel->GetSinkPort();
            _rf(router, f, in_channel, &f->la_route_set, false);
          }
        }
        else
        {
          f->la_route_set.Clear();
        }
      }

#ifdef TRACK_FLOWS
      ++_outstanding_credits[f->cl][output];
      _outstanding_classes[output][f->vc].push(f->cl);
#endif
      // if(GetID() == 45)
      // if(output == 5 && input ==3 && vc == 0 && match_vc == 0)
      //   cout<<GetSimTime()<<" f_dup "<<f->id<<endl;
      dest_buf->SendingFlit(f);

      _crossbar_flits.push_back(make_pair(-1, make_pair(f, make_pair(expanded_input, expanded_output))));
      if (_out_queue_credits.count(input) == 0)
      {
        _out_queue_credits.insert(make_pair(input, Credit::New()));
      }
      _out_queue_credits.find(input)->second->vc.insert(vc);

      if (cur_buf->Empty(vc))
      {
        if (f->tail)
        {
          cur_buf->SetState(vc, VC::idle);
        }
      }
      else
      {
        Flit *const nf = cur_buf->FrontFlit(vc);
        assert(nf);
        assert(nf->vc == vc);
        if (f->tail)   //This doesn't happen (generally)
        {
          assert(nf->head);
          if (_routing_delay)
          {
              cur_buf->SetState(vc, VC::routing);
              if (nf->mflag) {
                  _route_vcs_multi.push_back(make_pair(-1, item.second.first));
                  if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                  {
                      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                          << "  flit " << nf->id
                          << " will be sent to mcast routing in switch hold update " << endl;
                  }
              }

              else
              {
                  _route_vcs.push_back(make_pair(-1, item.second.first));
                  if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                  {
                      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                          << "  flit " << nf->id
                          << " will be sent to unicast routing in " << endl;
                  }
              }
          }
          else
          {
            if (nf->watch)
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "Using precomputed lookahead routing information for VC " << vc
                         << " at input " << input
                         << " (front: " << nf->id
                         << ")." << endl;
            }
            cur_buf->SetRouteSet(vc, &nf->la_route_set);
            cur_buf->SetState(vc, VC::vc_alloc);
            if (_speculative)
            {
              _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                              -1)));
            }
            if (_vc_allocator)
            {
              _vc_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                              -1)));
            }
            if (_noq)
            {
              _UpdateNOQ(input, vc, nf);
            }
          }
        }
        else  //Body flits
        {
          if (_hold_switch_for_packet)
          {
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
              *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                         << "Setting up switch hold for VC " << vc
                         << " at input " << input
                         << "." << (expanded_input % _input_speedup)
                         << " to output " << output
                         << "." << (expanded_output % _output_speedup)
                         << "." << endl;
            }
            _switch_hold_vc[expanded_input] = vc;
            _switch_hold_in[expanded_input] = expanded_output;
            _switch_hold_out[expanded_output] = expanded_input;
            _sw_hold_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                           -1)));
          }
          else
          {   //Body flit and tail flit gets pushed back here
            _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                            -1)));
          }
        }
      }
    }
    else
    {
      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  No output port allocated." << endl;
      }

#ifdef TRACK_STALLS
      assert((expanded_output == -1) || // for stalls that are accounted for in VC allocation path
             (expanded_output == STALL_BUFFER_BUSY) ||
             (expanded_output == STALL_BUFFER_CONFLICT) ||
             (expanded_output == STALL_BUFFER_FULL) ||
             (expanded_output == STALL_BUFFER_RESERVED) ||
             (expanded_output == STALL_CROSSBAR_CONFLICT));
      if (expanded_output == STALL_BUFFER_BUSY)
      {
        ++_buffer_busy_stalls[f->cl];
      }
      else if (expanded_output == STALL_BUFFER_CONFLICT)
      {
        ++_buffer_conflict_stalls[f->cl];
      }
      else if (expanded_output == STALL_BUFFER_FULL)
      {
        ++_buffer_full_stalls[f->cl];
      }
      else if (expanded_output == STALL_BUFFER_RESERVED)
      {
        ++_buffer_reserved_stalls[f->cl];
      }
      else if (expanded_output == STALL_CROSSBAR_CONFLICT)
      {
        ++_crossbar_conflict_stalls[f->cl];
      }
#endif

      _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second.first, -1)));
    }
    _sw_alloc_vcs.pop_front();
  }
}

void IQRouter::addFlitMCastEntry(int dest, int outport, int input , int vc, int wflag)
{

  Buffer * cur_buf = _buf[input];
  assert(!cur_buf->Empty(vc));
  cur_buf->addFlitMCastEntry(vc, dest, outport, wflag);
    
}

//------------------------------------------------------------------------------
// routing Multicast
//------------------------------------------------------------------------------

void IQRouter::_RouteEvaluateMulti()
{
  assert(_routing_delay);

  for (deque<pair<int, pair<int, int> > >::iterator iter = _route_vcs_multi.begin();
       iter != _route_vcs_multi.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }
    iter->first = GetSimTime() + _routing_delay - 1;

    int const input = iter->second.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.second;
    assert((vc >= 0) && (vc < _vcs));

    Buffer const *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::routing);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " mcast Beginning routing for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }
  }
}

void IQRouter::_RouteUpdateMulti()
{
  assert(_routing_delay);

  while (!_route_vcs_multi.empty())
  {

    pair<int, pair<int, int> > const &item = _route_vcs_multi.front();

    int const time = item.first;
    if ((time < 0) || (GetSimTime() < time))
    {
      break;
    }
    assert(GetSimTime() == time);

    int const input = item.second.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = item.second.second;
    assert((vc >= 0) && (vc < _vcs));

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::routing);

    Flit *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " mcast Completed routing for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }

    cur_buf->Route(vc, _rf, this, f, input);
    cur_buf->SetState(vc, VC::vc_alloc);
    // if (_speculative)
    // {
    //   _sw_alloc_vcs.push_back(make_pair(-1, make_pair(item.second, -1)));
    // }
    if (_vc_allocator)
    {
      // push multiple entries here for each outut port
      map<int, pair<vector<int>, vector<int> > > mcast_table = cur_buf->GetMcastTable(vc);
      //for(map<int, pair<vector<int>, vector<int> > >::iterator itr = mcast_table.begin();itr != mcast_table.end(); itr++)
      //{
        _vc_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second, make_pair(false,-1))));
      //}
    }
    // NOTE: No need to handle NOQ here, as it requires lookahead routing!
    _route_vcs_multi.pop_front();
  }
}

//------------------------------------------------------------------------------
// VC allocation Multicast
//------------------------------------------------------------------------------

void IQRouter::_VCAllocEvaluateMulti()
{
  assert(_vc_allocator);

  bool watched = false;
  vector<int> multioutputandvc;

  for (deque<pair<int, pair<pair<int, int>, pair<bool,int> > > >::iterator iter = _vc_alloc_vcs_multi.begin();
       iter != _vc_alloc_vcs_multi.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }

    int const input = iter->second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    // cout<<"rid "<<GetID()<<" simtime "<<GetSimTime()<<" outport "<<iter->second.second.first<<endl;
    assert(iter->second.second.second == -1);
    Buffer * const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert(cur_buf->GetState(vc) == VC::vc_alloc);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      // cout<<"TIme is "<<time<<endl;
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " mcast Beginning VC allocation for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }

    // OutputSet const *const route_set = cur_buf->GetRouteSet(vc);
    // assert(route_set);

    int const out_priority = cur_buf->GetPriority(vc);
    // set<OutputSet::sSetElement> const setlist = route_set->GetSet();

    bool elig = false;
    bool cred = false;
    bool reserved = false;

    // assert(!_noq || (setlist.size() == 1));

    // for (set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
    //      iset != setlist.end();
    //      ++iset)
    // {

    //int const out_port_number = iter->second.second.first;
    //assert((out_port >= 0) && (out_port < _outputs));
    int vc_acquired =0;
    map<int,int> empty_vc;
    for (auto& x : cur_buf->GetMcastTable(vc)) {
        BufferState const* const dest_buf = _next_buf[x.first];

        int vc_start;
        int vc_end;

        // if (_noq && _noq_next_output_port[input][vc] >= 0)
        // {
        //   assert(!_routing_delay);
        //   vc_start = _noq_next_vc_start[input][vc];
        //   vc_end = _noq_next_vc_end[input][vc];
        // }
        // else
        // {
        vc_start = 0;
        vc_end = _vcs - 1;
        // }
        assert(vc_start >= 0 && vc_start < _vcs);
        assert(vc_end >= 0 && vc_end < _vcs);
        assert(vc_end >= vc_start);

        for (int out_vc = vc_start; out_vc <= vc_end; ++out_vc)
        {
            assert((out_vc >= 0) && (out_vc < _vcs));

            int in_priority = 0;
            // if (_vc_prioritize_empty && !dest_buf->IsEmptyFor(out_vc))
            // {
            //   assert(in_priority >= 0);
            //   in_priority += numeric_limits<int>::min();
            // }

            // On the input input side, a VC might request several output VCs.
            // These VCs can be prioritized by the routing function, and this is
            // reflected in "in_priority". On the output side, if multiple VCs are
            // requesting the same output VC, the priority of VCs is based on the
            // actual packet priorities, which is reflected in "out_priority".

            if (!((dest_buf->AvailableFor(out_vc) - f->flits_num >= 0) && dest_buf->IsAvailableFor(out_vc)))
            {

                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                    int const use_input_and_vc = dest_buf->UsedBy(out_vc);
                    int const use_input = use_input_and_vc / _vcs;
                    int const use_vc = use_input_and_vc % _vcs;
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                        << " mcast VC " << out_vc
                        << " at output " << x.first
                        << " is in use by VC " << use_vc
                        << " at input " << use_input;
                    //Flit* cf = dest_buf[use_input]->FrontFlit(use_vc);
                    //*gWatchOut << " state is " << _buf[use_input]->GetState(use_vc);
                   // if (cf)
                    //{
                    //    *gWatchOut << " (front flit: " << cf->id << " mcast = " << cf->mflag << " head = " << cf->head << " tail = " << cf->tail << ")";
                   // }
                    //else
                    //{
                   //     *gWatchOut << " (empty)";
                    //}
                   // *gWatchOut << "." << endl;
                }

            }
            else
            {
                elig = true;
                if (_vc_busy_when_full && dest_buf->IsFullFor(out_vc))
                {
                    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                        << "  VC " << out_vc
                        << " at output " << x.first
                        << " is full. " << "last pid = " << dest_buf->_last_pid[out_vc] << "last fid = " << dest_buf->_last_id[out_vc] << endl;
                    dest_buf->Display(*gWatchOut);
                    reserved |= !dest_buf->IsFull();
                }
                else
                {
                    // int const time = iter->first;
                    // if (time >= 0)
                    // {
                    //   cout<<"time rid "<<GetID()<<endl;
                    //   break;
                    // }
                    // iter->first = GetSimTime() + _vc_alloc_delay - 1;
                    cred = true;

                    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                    {
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                            << " mcast Requesting VC " << out_vc
                            << " at output " << x.first
                            << " (in_pri: " << in_priority
                            << ", out_pri: " << out_priority
                            << ")." << endl;
                        watched = true;

                    }
                    vc_acquired += 1;
                    empty_vc[x.first]=out_vc;
                    break;
                }
            }
        }
    }
    if (vc_acquired == cur_buf->GetMcastTable(vc).size()) {
        // iter->second.second.second = out_port * _vcs + out_vc;
        for (auto& x : cur_buf->GetMcastTable(vc)) {
            BufferState* const dest_buf = _next_buf[x.first];
            assert(dest_buf->IsAvailableFor(empty_vc[x.first]));
            Flit* cf = _buf[input]->FrontFlit(vc);
            dest_buf->TakeBuffer(empty_vc[x.first], input * _vcs + vc);

            cur_buf->PushMOutputandVC(vc, x.first * _vcs + empty_vc[x.first]);
            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " << GetID()
                    << " mcast Assigning VC " << empty_vc[x.first]
                    << " at output " << x.first
                    << " to VC " << vc
                    << " at input " << input
                    << "." << endl;
                
            }
            iter->second.second.first = true;
            cur_buf->SetMCastCount(vc, 0);
        }
    }

          // int const input_and_vc = _vc_shuffle_requests ? (vc * _inputs + input) : (input * _vcs + vc);
          // _vc_allocator->AddRequest(input_and_vc, out_port * _vcs + out_vc,
                                    // 0, in_priority, out_priority);
      
    if (!elig)
    {
      iter->second.second.second = STALL_BUFFER_BUSY;
    }
    else if (_vc_busy_when_full && !cred)
    {
      iter->second.second.second = reserved ? STALL_BUFFER_RESERVED : STALL_BUFFER_FULL;
    }
    
  }

  // if (watched)
  // {
  //   *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | " << " rid = " <<GetID() ;
  //   _vc_allocator->PrintRequests(gWatchOut);
  // }

  // _vc_allocator->Allocate();

  // if (watched)
  // {
  //   *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | " << " rid = " <<GetID() ;
  //   _vc_allocator->PrintGrants(gWatchOut);
  // }

  for (deque<pair<int, pair<pair<int, int>, pair<bool, int> > > >::iterator iter = _vc_alloc_vcs_multi.begin();
      iter != _vc_alloc_vcs_multi.end();
      ++iter)
  {

      int const time = iter->first;
      if (time >= 0)
      {
          break;
      }
      iter->first = GetSimTime() + _vc_alloc_delay - 1;
  }
  /*
    int const input = iter->second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    if (iter->second.second.second < -1)
    {
      continue;
    }

    assert(iter->second.second.second == -1);

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    // if(cur_buf->GetMCastCount() == cur_buf->GetMcastTable().size)
    assert(cur_buf->GetState(vc) == VC::vc_alloc);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    int const input_and_vc = _vc_shuffle_requests ? (vc * _inputs + input) : (input * _vcs + vc);
    vector <int > Outpairs = cur_buf->GetMulticastOutpair(vc);
    int output_and_vc = -1;
    for(int i = 0 ; i < Outpairs.size() ; i++)
      if ( Outpairs[i]/_vcs == iter->second.second.first ) {
        output_and_vc = Outpairs[i];
        break;
      }
      

    if (output_and_vc >= 0)
    {

      int const match_output = output_and_vc / _vcs;
      assert((match_output >= 0) && (match_output < _outputs));
      int const match_vc = output_and_vc % _vcs;
      assert((match_vc >= 0) && (match_vc < _vcs));

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " mcast Assigning VC " << match_vc
                   << " at output " << match_output
                   << " to VC " << vc
                   << " at input " << input
                   << "." << endl;
      }
      // cout<<" assigned vc"<<endl;
      iter->second.second.second = output_and_vc;
      
    }
    else
    {

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " mcast VC allocation failed for VC " << vc
                   << " at input " << input
                   << "." << endl;
      }

      iter->second.second.second = STALL_BUFFER_CONFLICT;
    }
  }

  if (_vc_alloc_delay <= 1)
  {
    return;
  }*/

  // for (deque<pair<int, pair<pair<int, int>, pair<int, int> > > >::iterator iter = _vc_alloc_vcs_multi.begin();
  //      iter != _vc_alloc_vcs_multi.end();
  //      ++iter)
  // {

  //   int const time = iter->first;
  //   assert(time >= 0);
  //   if (GetSimTime() < time)
  //   {
  //     break;
  //   }

  //   assert(iter->second.second.second != -1);

  //   int const output_and_vc = iter->second.second.second;

  //   if (output_and_vc >= 0)
  //   {

  //     int const match_output = output_and_vc / _vcs;
  //     assert((match_output >= 0) && (match_output < _outputs));
  //     int const match_vc = output_and_vc % _vcs;
  //     assert((match_vc >= 0) && (match_vc < _vcs));

  //     BufferState const *const dest_buf = _next_buf[match_output];

  //     int const input = iter->second.first.first;
  //     assert((input >= 0) && (input < _inputs));
  //     int const vc = iter->second.first.second;
  //     assert((vc >= 0) && (vc < _vcs));

  //     Buffer const *const cur_buf = _buf[input];
  //     assert(!cur_buf->Empty(vc));
  //     assert(cur_buf->GetState(vc) == VC::vc_alloc);

  //     Flit const *const f = cur_buf->FrontFlit(vc);
  //     assert(f);
  //     assert(f->vc == vc);
  //     assert(f->head);

  //     if (!dest_buf->IsAvailableFor(match_vc))
  //     {
  //       if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //       {
  //         *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                    << "  Discarding previously generated grant for VC " << vc
  //                    << " at input " << input
  //                    << ": VC " << match_vc
  //                    << " at output " << match_output
  //                    << " is no longer available." << endl;
  //       }
  //       iter->second.second.second = STALL_BUFFER_BUSY;
  //     }
  //     else if (_vc_busy_when_full && dest_buf->IsFullFor(match_vc))
  //     {
  //       if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //       {
  //         *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                    << "  Discarding previously generated grant for VC " << vc
  //                    << " at input " << input
  //                    << ": VC " << match_vc
  //                    << " at output " << match_output
  //                    << " has become full." << endl;
  //       }
  //       iter->second.second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
  //     }
  //   }
  // }
}


void IQRouter::_VCAllocUpdateMulti()
{
  assert(_vc_allocator);
  //set<int> finish_invc;
  while (!_vc_alloc_vcs_multi.empty())
  {

    pair<int, pair<pair<int, int>, pair<int, int> > > const &item = _vc_alloc_vcs_multi.front();

    int const time = item.first;
    if ((time < 0) || (GetSimTime() < time))
    {
      // cout<<item.second.second.second<<endl;
      break;
    }
    assert(GetSimTime() == time);

    int const input = item.second.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = item.second.first.second;
    assert((vc >= 0) && (vc < _vcs));

    //assert(item.second.second.second != -1);

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));

    if(cur_buf->GetMulticastOutpair(vc).size() != cur_buf->GetMcastTable(vc).size())
      assert(cur_buf->GetState(vc) == VC::vc_alloc);

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);
    assert(f->head);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " mcast Completed VC allocation for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << ")." << endl;
    }

    //int const output_and_vc = item.second.second.second;

    if (item.second.second.first)
    {/*
      int const match_output = output_and_vc / _vcs;
      assert((match_output >= 0) && (match_output < _outputs));
      int const match_vc = output_and_vc % _vcs;
      assert((match_vc >= 0) && (match_vc < _vcs));

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " mcast Acquiring assigned VC " << match_vc
                   << " at output " << match_output
                   << "." << endl;
      }*/

      //BufferState *const dest_buf = _next_buf[match_output];
        assert(cur_buf->GetMulticastOutpair(vc).size() == cur_buf->GetMcastTable(vc).size());
            cur_buf->SetState(vc, VC::active);
//            if(finish_invc.count(item.second.first.first * _vcs + item.second.first.second)==0)
            for (int i = 0; i < cur_buf->GetMulticastOutpair(vc).size(); i++) {
                _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(make_pair(item.second.first, cur_buf->GetMulticastOutpair(vc)[i]), -1)));
                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                    *gWatchOut << " push into switch alloc vc_alloc " << f->id << " Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                }
            }
 //           finish_invc.insert(item.second.first.first * _vcs + item.second.first.second);   
      if (!_speculative)
      {
//          _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(make_pair(item.second.first,output_and_vc), -1)));
      
        // cout<<"pushin into switch alloc vcalloc"<<f->id<<endl;
      }
    }
    else
    {
      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  No output VC allocated." << endl;
      }

#ifdef TRACK_STALLS
      assert((output_and_vc == STALL_BUFFER_BUSY) ||
             (output_and_vc == STALL_BUFFER_CONFLICT));
      if (output_and_vc == STALL_BUFFER_BUSY)
      {
        ++_buffer_busy_stalls[f->cl];
      }
      else if (output_and_vc == STALL_BUFFER_CONFLICT)
      {
        ++_buffer_conflict_stalls[f->cl];
      }
#endif
      // cout<<"pushing again into vcalloc rid "<<GetID()<<endl;
      assert(!item.second.second.first);
      _vc_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first, make_pair(item.second.second.first,-1))));

    }
    _vc_alloc_vcs_multi.pop_front();
  }
}



//------------------------------------------------------------------------------
// switch allocation multicast
//------------------------------------------------------------------------------

bool IQRouter::_SWAllocAddReqMulti(int input, int vc, int output)
{
  assert(input >= 0 && input < _inputs);
  assert(vc >= 0 && vc < _vcs);
  assert(output >= 0 && output < _outputs);

  // When input_speedup > 1, the virtual channel buffers are interleaved to
  // create multiple input ports to the switch. Similarily, the output ports
  // are interleaved based on their originating input when output_speedup > 1.

  int const expanded_input = input * _input_speedup + vc % _input_speedup;
  int const expanded_output = output * _output_speedup + input % _output_speedup;

  Buffer *const cur_buf = _buf[input];
  assert(!cur_buf->Empty(vc));
  
  if(cur_buf->GetMCastCount(vc) == cur_buf->GetMcastTable(vc).size())
  {
    assert((cur_buf->GetState(vc) == VC::active) ||
          (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));
  }

  Flit const *const f = cur_buf->FrontFlit(vc);
  assert(f);
  assert(f->vc == vc);

  if ((_switch_hold_in[expanded_input] < 0) &&
      (_switch_hold_out[expanded_output] < 0))
  {

    Allocator *allocator = _sw_allocator;
    int prio = cur_buf->GetPriority(vc);

    if (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc))
    {
      if (_spec_sw_allocator)
      {
        allocator = _spec_sw_allocator;
      }
      else
      {
        assert(prio >= 0);
        prio += numeric_limits<int>::min();
      }
    }

    Allocator::sRequest req;
  //Following block is skipped for the first iter
    if (allocator->ReadRequest(req, expanded_input, expanded_output))
    {
      if (RoundRobinArbiter::Supersedes(vc, prio, req.label, req.in_pri,
                                        _sw_rr_offset[expanded_input], _vcs)) //Only body and tail
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  Replacing earlier request from VC " << req.label
                     << " for output " << output
                     << "." << (expanded_output % _output_speedup)
                     << " with priority " << req.in_pri
                     << " (" << ((cur_buf->GetState(vc) == VC::active) ? "non-spec" : "spec")
                     << ", pri: " << prio
                     << ")." << endl;
        }
        allocator->RemoveRequest(expanded_input, expanded_output, req.label);
        allocator->AddRequest(expanded_input, expanded_output, vc, prio, prio);
        return true;
      }

      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Output " << output
                   << "." << (expanded_output % _output_speedup)
                   << " was already requested by VC " << req.label
                   << " with priority " << req.in_pri
                   << " (pri: " << prio
                   << ")." << endl;
      }
      return false;
    }

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << "  Requesting output " << output
                 << "." << (expanded_output % _output_speedup)
                 << " (" << ((cur_buf->GetState(vc) == VC::active) ? "non-spec" : "spec")
                 << ", pri: " << prio
                 << ")." << endl;
    }
    allocator->AddRequest(expanded_input, expanded_output, vc, prio, prio);
    return true;
  }

  if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  {
    *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
               << "  Ignoring output " << output
               << "." << (expanded_output % _output_speedup)
               << " due to switch hold (";
    if (_switch_hold_in[expanded_input] >= 0)
    {
      *gWatchOut << "input: " << input
                 << "." << (expanded_input % _input_speedup);
      if (_switch_hold_out[expanded_output] >= 0)
      {
        *gWatchOut << ", ";
      }
    }
    if (_switch_hold_out[expanded_output] >= 0)
    {
      *gWatchOut << "output: " << output
                 << "." << (expanded_output % _output_speedup);
    }
    *gWatchOut << ")." << endl;
  }
  return false;
}


void IQRouter::_SWAllocEvaluateMulti()
{
  bool watched = false;

  for (deque<pair<int, pair<pair<pair<int, int>, int >,int> > >::iterator iter = _sw_alloc_vcs_multi.begin();
       iter != _sw_alloc_vcs_multi.end();
       ++iter)
  {
    
    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }

    int const input = iter->second.first.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.first.second;
    assert((vc >= 0) && (vc < _vcs));

    assert(iter->second.second == -1);

    assert(_switch_hold_vc[input * _input_speedup + vc % _input_speedup] != vc);

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    if(cur_buf->GetMCastCount(vc) == cur_buf->GetMcastTable(vc).size())
    {
      assert((cur_buf->GetState(vc) == VC::active) ||
            (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));
    }

    Flit const *const f = cur_buf->FrontFlit(vc);

    assert(f);
    assert(f->vc == vc);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " mcast Beginning switch allocation for VC " << vc
                 << " at input " << input
                 << " (front: " << f->id
                 << "). Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
    }

    // if (cur_buf->GetState(vc) == VC::active) //all flits go here and then just calls continue
    // {
      int outputandvc = iter->second.first.second;
      int const dest_output = outputandvc / _vcs;
      assert((dest_output >= 0) && (dest_output < _outputs));
      int const dest_vc = outputandvc % _vcs;
      assert((dest_vc >= 0) && (dest_vc < _vcs));

      BufferState const *const dest_buf = _next_buf[dest_output];

      if (dest_buf->IsFullFor(dest_vc) || (_output_buffer_size != -1 && _output_buffer[dest_output].size() >= (size_t)(_output_buffer_size)))
      {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << " mcast VC " << dest_vc
                     << " at output " << dest_output
                     << " is full. "<<"last pid = " << dest_buf->_last_pid[dest_vc] << "last fid = " << dest_buf->_last_id[dest_vc] << endl;
          dest_buf->Display(*gWatchOut);
          *gWatchOut << GetSimTime() << " | " << dest_buf->IsFullFor(dest_vc) << " || "
                     << "  outbufsize " << _output_buffer_size
                     << " output_buffer[dest_output].size() " << _output_buffer[dest_output].size()
                     << endl;
          
        }
        iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
        continue;
      }
      bool const requested = _SWAllocAddReqMulti(input, vc, dest_output);
      watched |= requested && f->watch;
      continue;
    // }
    // assert(_speculative && (cur_buf->GetState(vc) == VC::vc_alloc));
    // assert(f->head);

    // // The following models the speculative VC allocation aspects of the
    // // pipeline. An input VC with a request in for an egress virtual channel
    // // will also speculatively bid for the switch regardless of whether the VC
    // // allocation succeeds.

    // OutputSet const *const route_set = cur_buf->GetRouteSet(vc);
    // assert(route_set);

    // set<OutputSet::sSetElement> const setlist = route_set->GetSet();

    // assert(!_noq || (setlist.size() == 1));

    // for (set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
    //      iset != setlist.end();
    //      ++iset)
    // {

    //   int const dest_output = iset->output_port;
    //   assert((dest_output >= 0) && (dest_output < _outputs));

    //   // for lower levels of speculation, ignore credit availability and always
    //   // issue requests for all output ports in route set

    //   BufferState const *const dest_buf = _next_buf[dest_output];

    //   bool elig = false;
    //   bool cred = false;

    //   if (_spec_check_elig)
    //   {

    //     // for higher levels of speculation, check if at least one suitable VC
    //     // is available at the current output

    //     int vc_start;
    //     int vc_end;

    //     if (_noq && _noq_next_output_port[input][vc] >= 0)
    //     {
    //       assert(!_routing_delay);
    //       vc_start = _noq_next_vc_start[input][vc];
    //       vc_end = _noq_next_vc_end[input][vc];
    //     }
    //     else
    //     {
    //       vc_start = iset->vc_start;
    //       vc_end = iset->vc_end;
    //     }
    //     assert(vc_start >= 0 && vc_start < _vcs);
    //     assert(vc_end >= 0 && vc_end < _vcs);
    //     assert(vc_end >= vc_start);

    //     for (int dest_vc = vc_start; dest_vc <= vc_end; ++dest_vc)
    //     {
    //       assert((dest_vc >= 0) && (dest_vc < _vcs));

    //       if (dest_buf->IsAvailableFor(dest_vc) && (_output_buffer_size == -1 || _output_buffer[dest_output].size() < (size_t)(_output_buffer_size)))
    //       {
    //         elig = true;
    //         if (!_spec_check_cred || !dest_buf->IsFullFor(dest_vc))
    //         {
    //           cred = true;
    //           break;
    //         }
    //       }
    //     }
    //   }

    //   if (_spec_check_elig && !elig)
    //   {
    //     if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    //     {
    //       *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
    //                  << "  Output " << dest_output
    //                  << " has no suitable VCs available." << endl;
    //     }
    //     iter->second.second = STALL_BUFFER_BUSY;
    //   }
    //   else if (_spec_check_cred && !cred)
    //   {
    //     if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    //     {
    //       *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
    //                  << "  All suitable VCs at output " << dest_output
    //                  << " are full." << endl;
    //     }
    //     iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
    //   }
    //   else
    //   {
    //     bool const requested = _SWAllocAddReq(input, vc, dest_output);
    //     watched |= requested && f->watch;
    //   }
    // }
  }

  // if (watched)
  // {
  //   *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
  //   _sw_allocator->PrintRequests(gWatchOut);
  //   if (_spec_sw_allocator)
  //   {
  //     *gWatchOut << GetSimTime() << " | " << _spec_sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
  //     _spec_sw_allocator->PrintRequests(gWatchOut);
  //   }
  // }

  // _sw_allocator->Allocate();
  // if (_spec_sw_allocator)
  //   _spec_sw_allocator->Allocate();

  // if (watched)
  // {
  //   *gWatchOut << GetSimTime() << " | " << _sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
  //   _sw_allocator->PrintGrants(gWatchOut);
  //   if (_spec_sw_allocator)
  //   {
  //     *gWatchOut << GetSimTime() << " | " << _spec_sw_allocator->FullName() << " | " << " rid = " <<GetID() ;
  //     _spec_sw_allocator->PrintGrants(gWatchOut);
  //   }
  // }

  for (deque<pair<int, pair<pair<pair<int, int>, int >,int> > >::iterator iter = _sw_alloc_vcs_multi.begin();
       iter != _sw_alloc_vcs_multi.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }
    iter->first = GetSimTime() + _sw_alloc_delay - 1;

    int const input = iter->second.first.first.first;
    assert((input >= 0) && (input < _inputs));
    int const vc = iter->second.first.first.second;
    assert((vc >= 0) && (vc < _vcs));

    if (iter->second.second < -1)
    {
      continue;
    }

    assert(iter->second.second == -1);

    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    if(cur_buf->GetMCastCount(vc) == cur_buf->GetMcastTable(vc).size())
    {
      assert((cur_buf->GetState(vc) == VC::active) ||
            (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));
    }

    Flit const *const f = cur_buf->FrontFlit(vc);
    assert(f);
    assert(f->vc == vc);

    int const expanded_input = input * _input_speedup + vc % _input_speedup;

    // int expanded_output = _sw_allocator->OutputAssigned(expanded_input);
    int expanded_output = (iter->second.first.second / _vcs) * _output_speedup;


    if (expanded_output >= 0)
    {
      assert((expanded_output % _output_speedup) == (input % _output_speedup));
      // int const granted_vc = _sw_allocator->ReadRequest(expanded_input, expanded_output);
      // if (granted_vc == vc)
      // {
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << " mcast Assigning output " << (expanded_output / _output_speedup)
                     << "." << (expanded_output % _output_speedup)
                     << " to VC " << vc
                     << " at input " << input
                     << "." << (vc % _input_speedup)
                     << "." << endl;
        }
        _sw_rr_offset[expanded_input] = (vc + _input_speedup) % _vcs;
        iter->second.second = expanded_output;
      // }
      // else
      // {
      //   if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      //   {
      //     *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
      //                << "Switch allocation failed for VC " << vc
      //                << " at input " << input
      //                << ": Granted to VC " << granted_vc << "." << endl;
      //   }
      //   iter->second.second = STALL_CROSSBAR_CONFLICT;
      // }
    }
    // else if (_spec_sw_allocator)
    // {
    //   expanded_output = _spec_sw_allocator->OutputAssigned(expanded_input);
    //   if (expanded_output >= 0)
    //   {
    //     assert((expanded_output % _output_speedup) == (input % _output_speedup));
    //     if (_spec_mask_by_reqs &&
    //         _sw_allocator->OutputHasRequests(expanded_output))
    //     {
    //       if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    //       {
    //         *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
    //                    << " Discarding speculative grant for VC " << vc
    //                    << " at input " << input
    //                    << "." << (vc % _input_speedup)
    //                    << " because output " << (expanded_output / _output_speedup)
    //                    << "." << (expanded_output % _output_speedup)
    //                    << " has non-speculative requests." << endl;
    //       }
    //       iter->second.second = STALL_CROSSBAR_CONFLICT;
    //     }
    //     else if (!_spec_mask_by_reqs &&
    //              (_sw_allocator->InputAssigned(expanded_output) >= 0))
    //     {
    //       if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    //       {
    //         *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
    //                    << " Discarding speculative grant for VC " << vc
    //                    << " at input " << input
    //                    << "." << (vc % _input_speedup)
    //                    << " because output " << (expanded_output / _output_speedup)
    //                    << "." << (expanded_output % _output_speedup)
    //                    << " has a non-speculative grant." << endl;
    //       }
    //       iter->second.second = STALL_CROSSBAR_CONFLICT;
    //     }
    //     else
    //     {
    //       int const granted_vc = _spec_sw_allocator->ReadRequest(expanded_input,
    //                                                              expanded_output);
    //       if (granted_vc == vc)
    //       {
    //         if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    //         {
    //           *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
    //                      << " assigning output " << (expanded_output / _output_speedup)
    //                      << "." << (expanded_output % _output_speedup)
    //                      << " to VC " << vc
    //                      << " at input " << input
    //                      << "." << (vc % _input_speedup)
    //                      << "." << endl;
    //         }
    //         _sw_rr_offset[expanded_input] = (vc + _input_speedup) % _vcs;
    //         iter->second.second = expanded_output;
    //       }
    //       else
    //       {
    //         if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    //         {
    //           *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
    //                      << "Switch allocation failed for VC " << vc
    //                      << " at input " << input
    //                      << ": Granted to VC " << granted_vc << "." << endl;
    //         }
    //         iter->second.second = STALL_CROSSBAR_CONFLICT;
    //       }
    //     }
    //   }
    //   else
    //   {

    //     if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    //     {
    //       *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
    //                  << "Switch allocation failed for VC " << vc
    //                  << " at input " << input
    //                  << ": No output granted." << endl;
    //     }

    //     iter->second.second = STALL_CROSSBAR_CONFLICT;
    //   }
    // }
    else
    {
      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " mcast Switch allocation failed for VC " << vc
                   << " at input " << input
                   << ": No output granted." << endl;
      }

      iter->second.second = STALL_CROSSBAR_CONFLICT;
    }
  }

  if (!_speculative && (_sw_alloc_delay <= 1))
  {
    return;
  }

  // for (deque<pair<int, pair<pair<pair<int, int>, int >,int> > >::iterator iter = _sw_alloc_vcs_multi.begin();
  //      iter != _sw_alloc_vcs_multi.end();
  //      ++iter)
  // {
  //   int const time = iter->first;
  //   assert(time >= 0);
  //   if (GetSimTime() < time)
  //   {
  //     break;
  //   }

  //   assert(iter->second.second != -1);

  //   int const expanded_output = iter->second.second;

  //   if (expanded_output >= 0)
  //   {

  //     int const output = expanded_output / _output_speedup;
  //     assert((output >= 0) && (output < _outputs));

  //     BufferState const *const dest_buf = _next_buf[output];

  //     int const input = iter->second.first.first.first;
  //     assert((input >= 0) && (input < _inputs));
  //     assert((input % _output_speedup) == (expanded_output % _output_speedup));
  //     int const vc = iter->second.first.first.second;
  //     assert((vc >= 0) && (vc < _vcs));

  //     int const expanded_input = input * _input_speedup + vc % _input_speedup;
  //     assert(_switch_hold_vc[expanded_input] != vc);

  //     Buffer const *const cur_buf = _buf[input];
  //     assert(!cur_buf->Empty(vc));
  //     assert((cur_buf->GetState(vc) == VC::active) ||
  //            (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));

  //     Flit const *const f = cur_buf->FrontFlit(vc);
  //     assert(f);
  //     assert(f->vc == vc);

  //     if ((_switch_hold_in[expanded_input] >= 0) ||
  //         (_switch_hold_out[expanded_output] >= 0))
  //     {
  //       if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //       {
  //         *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                    << " Discarding grant from input " << input
  //                    << "." << (vc % _input_speedup)
  //                    << " to output " << output
  //                    << "." << (expanded_output % _output_speedup)
  //                    << " due to conflict with held connection at ";
  //         if (_switch_hold_in[expanded_input] >= 0)
  //         {
  //           *gWatchOut << "input";
  //         }
  //         if ((_switch_hold_in[expanded_input] >= 0) &&
  //             (_switch_hold_out[expanded_output] >= 0))
  //         {
  //           *gWatchOut << " and ";
  //         }
  //         if (_switch_hold_out[expanded_output] >= 0)
  //         {
  //           *gWatchOut << "output";
  //         }
  //         *gWatchOut << "." << endl;
  //       }
  //       iter->second.second = STALL_CROSSBAR_CONFLICT;
  //     }
  //     else if (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc))
  //     {

  //       assert(f->head);

  //       if (_vc_allocator)
  //       { // separate VC and switch allocators

  //         int const input_and_vc =
  //             _vc_shuffle_requests ? (vc * _inputs + input) : (input * _vcs + vc);
  //         int const output_and_vc = _vc_allocator->OutputAssigned(input_and_vc);

  //         if (output_and_vc < 0)
  //         {
  //           if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //           {
  //             *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                        << " Discarding grant from input " << input
  //                        << "." << (vc % _input_speedup)
  //                        << " to output " << output
  //                        << "." << (expanded_output % _output_speedup)
  //                        << " due to misspeculation." << endl;
  //           }
  //           iter->second.second = -1; // stall is counted in VC allocation path!
  //         }
  //         else if ((output_and_vc / _vcs) != output)
  //         {
  //           if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //           {
  //             *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                        << " Discarding grant from input " << input
  //                        << "." << (vc % _input_speedup)
  //                        << " to output " << output
  //                        << "." << (expanded_output % _output_speedup)
  //                        << " due to port mismatch between VC and switch allocator." << endl;
  //           }
  //           iter->second.second = STALL_BUFFER_CONFLICT; // count this case as if we had failed allocation
  //         }
  //         else if (dest_buf->IsFullFor((output_and_vc % _vcs)))
  //         {
  //           if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //           {
  //             *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                        << " Discarding grant from input " << input
  //                        << "." << (vc % _input_speedup)
  //                        << " to output " << output
  //                        << "." << (expanded_output % _output_speedup)
  //                        << " due to lack of credit." << endl;
  //           }
  //           iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
  //         }
  //       }
  //       else
  //       { // VC allocation is piggybacked onto switch allocation

  //         OutputSet const *const route_set = cur_buf->GetRouteSet(vc);
  //         assert(route_set);

  //         set<OutputSet::sSetElement> const setlist = route_set->GetSet();

  //         bool busy = true;
  //         bool full = true;
  //         bool reserved = false;

  //         assert(!_noq || (setlist.size() == 1));

  //         for (set<OutputSet::sSetElement>::const_iterator iset = setlist.begin();
  //              iset != setlist.end();
  //              ++iset)
  //         {
  //           if (iset->output_port == output)
  //           {

  //             int vc_start;
  //             int vc_end;

  //             if (_noq && _noq_next_output_port[input][vc] >= 0)
  //             {
  //               assert(!_routing_delay);
  //               vc_start = _noq_next_vc_start[input][vc];
  //               vc_end = _noq_next_vc_end[input][vc];
  //             }
  //             else
  //             {
  //               vc_start = iset->vc_start;
  //               vc_end = iset->vc_end;
  //             }
  //             assert(vc_start >= 0 && vc_start < _vcs);
  //             assert(vc_end >= 0 && vc_end < _vcs);
  //             assert(vc_end >= vc_start);

  //             for (int out_vc = vc_start; out_vc <= vc_end; ++out_vc)
  //             {
  //               assert((out_vc >= 0) && (out_vc < _vcs));
  //               if (dest_buf->IsAvailableFor(out_vc))
  //               {
  //                 busy = false;
  //                 if (!dest_buf->IsFullFor(out_vc))
  //                 {
  //                   full = false;
  //                   break;
  //                 }
  //                 else if (!dest_buf->IsFull())
  //                 {
  //                   reserved = true;
  //                 }
  //               }
  //             }
  //             if (!full)
  //             {
  //               break;
  //             }
  //           }
  //         }

  //         if (busy)
  //         {
  //           if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //           {
  //             *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                        << " Discarding grant from input " << input
  //                        << "." << (vc % _input_speedup)
  //                        << " to output " << output
  //                        << "." << (expanded_output % _output_speedup)
  //                        << " because no suitable output VC for piggyback allocation is available." << endl;
  //           }
  //           iter->second.second = STALL_BUFFER_BUSY;
  //         }
  //         else if (full)
  //         {
  //           if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //           {
  //             *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                        << " Discarding grant from input " << input
  //                        << "." << (vc % _input_speedup)
  //                        << " to output " << output
  //                        << "." << (expanded_output % _output_speedup)
  //                        << " because all suitable output VCs for piggyback allocation are full." << endl;
  //           }
  //           iter->second.second = reserved ? STALL_BUFFER_RESERVED : STALL_BUFFER_FULL;
  //         }
  //       }
  //     }
  //     else
  //     {
  //       assert(cur_buf->GetOutputPort(vc) == output);

  //       int const match_vc = cur_buf->GetOutputVC(vc);
  //       assert((match_vc >= 0) && (match_vc < _vcs));

  //       if (dest_buf->IsFullFor(match_vc))
  //       {
  //         if (f->watch || (_routers_to_watch.count(GetID()) > 0))
  //         {
  //           *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
  //                      << "  Discarding grant from input " << input
  //                      << "." << (vc % _input_speedup)
  //                      << " to output " << output
  //                      << "." << (expanded_output % _output_speedup)
  //                      << " due to lack of credit." << endl;
  //         }
  //         iter->second.second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
  //       }
  //     }
  //   }
  // }
}


void IQRouter::_SWAllocUpdateMulti()
{
    while (!_sw_alloc_vcs_multi.empty())
    {

        pair<int, pair<pair<pair<int, int>, int >, int> > const& item = _sw_alloc_vcs_multi.front();

        int const time = item.first;
        if ((time < 0) || (GetSimTime() < time))
        {
            break;
        }
        assert(GetSimTime() == time);

        int mcount;
        int const input = item.second.first.first.first;
        assert((input >= 0) && (input < _inputs));
        int const vc = item.second.first.first.second;
        assert((vc >= 0) && (vc < _vcs));

        Buffer* const cur_buf = _buf[input];
        Flit* const f = cur_buf->FrontFlit(vc);
        //if(cur_buf->GetMcastTable(vc).size()!=0 && f->mflag ==1){
        // cout<<"GetMcastTable(vc).size "<< cur_buf->GetMcastTable(vc).size() <<endl;
        assert(!cur_buf->Empty(vc));

        if (cur_buf->GetMCastCount(vc) == cur_buf->GetMcastTable(vc).size())
        {
            assert((cur_buf->GetState(vc) == VC::active) ||
                (_speculative && (cur_buf->GetState(vc) == VC::vc_alloc)));
        }


        assert(f);
        assert(f->mflag == 1);
        assert(f->vc == vc);

        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                << " mcast Completed switch allocation for VC " << vc
                << " at input " << input
                << " (front fid: " << f->id << " pid =" << f->pid << " mflag =" << f->mflag
                << ")." << endl;
        }

        int const expanded_output = item.second.second;

        if (expanded_output >= 0)
        {

            int const expanded_input = input * _input_speedup + vc % _input_speedup;
            assert(_switch_hold_vc[expanded_input] < 0);
            assert(_switch_hold_in[expanded_input] < 0);
            assert(_switch_hold_out[expanded_output] < 0);

            int const output = expanded_output / _output_speedup;
            assert((output >= 0) && (output < _outputs));

            BufferState* const dest_buf = _next_buf[output];

            int match_vc;



            match_vc = item.second.first.second % _vcs;
            mcount = cur_buf->GetMCastCount(vc);
            cur_buf->SetMCastCount(vc, ++mcount);
            // }
            assert((match_vc >= 0) && (match_vc < _vcs));

            if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            {
                *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                    << " mcast Scheduling switch connection from input " << input
                    << "." << (vc % _input_speedup)
                    << " to output " << output
                    << "." << (expanded_output % _output_speedup)
                    << "." << endl;
            }
            Flit* f_dup;
            if (mcount == cur_buf->GetMcastTable(vc).size())
            {

                cur_buf->RemoveFlit(vc);
                // cur_buf->SetMCastCount(vc, 0);
                f_dup = f;
                f_dup = _Generate_Duplicates(f, output, false);
                // cout<<"source router "<<f_dup->src<<" original id "<<f->id<<" f_dup id "<<f_dup->id<<" output "<<output<<" rid "<<GetID()<<endl;
                if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0)) {
                    *gWatchOut << GetSimTime() << " | "
                        << " Egress original flit " << f_dup->id
                        << " (packet " << f_dup->pid << " mflag = " << f_dup->mflag
                        << ") at output port" << output
                        << "." << endl;
                }
            }
            else
            {
                f_dup = _Generate_Duplicates(f, output, true);
                // cout<<"source router "<<f_dup->src<<" original id "<<f->id<<" f_dup id "<<f_dup->id<<" output "<<output<<" rid "<<GetID()<<endl;
                if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0)) {
                    *gWatchOut << GetSimTime() << " | "
                        << " Egress Duplicate flit " << f_dup->id
                        << " (packet " << f_dup->pid << " mflag = " << f_dup->mflag
                        << ") at output port" << output
                        << "." << endl;
                }
            }
            f_dup->mdest = cur_buf->GetMcastTable(vc)[output];
            /*
            if (f_dup->mdest.first.size() == 1 && f_dup->mdest.second.size() == 0) {
                f_dup->dest = f_dup->mdest.first[0];
                f_dup->mflag = false;
            }*/
            if (f_dup->head && f_dup->watch || (_routers_to_watch.count(GetID()) > 0))
            {


                *gWatchOut << " Dup_Pid " << f_dup->pid << " Destinations are: ";
                for (int i = 0; i < f_dup->mdest.first.size(); i++) {
                    *gWatchOut << f_dup->mdest.first[i] << " " << endl;
                }
                *gWatchOut << "num dests " << f_dup->mdest.first.size() << " simtime " << GetSimTime() << " source " << f_dup->src << endl;
            }

#ifdef TRACK_FLOWS
            --_stored_flits[f->cl][input];
            if (f->tail)
                --_active_packets[f->cl][input];
#endif

            _bufferMonitor->read(input, f);

            f_dup->hops++;
            f_dup->vc = match_vc;
            /*
            if (cur_buf->GetOutputPort(vc) == 5) //5 is the port to hub
            {
                //Bransan Statistacks
                int rid = hub_mapper[GetID()].second;

                if (f->head)
                {
                    wait_clock[rid].insert(make_pair(f->pid, GetSimTime()));
                    _cur_inter_hub = hub_mapper[f->dest].second;
                    cur_buf->SetInterDest(vc, _cur_inter_hub);
                }
                f->inter_dest = cur_buf->GetInterDest(vc);

                if (f->tail)
                {

                    assert(wait_clock[rid].find(f->pid) != wait_clock[rid].end());
                    time_and_cnt[rid].first += (GetSimTime() - wait_clock[rid][f->pid]);
                    time_and_cnt[rid].second++;
                    wait_clock[rid].erase(f->pid);

                    cur_buf->SetInterDest(vc, -1);
                }
            }*/
            // if (!_routing_delay && f_dup->head)
            // {
            //   const FlitChannel *channel = _output_channels[output];
            //   const Router *router = channel->GetSink();
            //   if (router)
            //   {
            //     if (_noq)
            //     {
            //       if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            //       {
            //         *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
            //                    << "Updating lookahead routing information for flit " << f->id
            //                    << " (NOQ)." << endl;
            //       }
            //       int next_output_port = _noq_next_output_port[input][vc];
            //       assert(next_output_port >= 0);
            //       _noq_next_output_port[input][vc] = -1;
            //       int next_vc_start = _noq_next_vc_start[input][vc];
            //       assert(next_vc_start >= 0 && next_vc_start < _vcs);
            //       _noq_next_vc_start[input][vc] = -1;
            //       int next_vc_end = _noq_next_vc_end[input][vc];
            //       assert(next_vc_end >= 0 && next_vc_end < _vcs);
            //       _noq_next_vc_end[input][vc] = -1;
            //       f->la_route_set.Clear();
            //       f->la_route_set.AddRange(next_output_port, next_vc_start, next_vc_end);
            //     }
            //     else
            //     {
            //       if (f->watch || (_routers_to_watch.count(GetID()) > 0))
            //       {
            //         *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
            //                    << "Updating lookahead routing information for flit " << f->id
            //                    << "." << endl;
            //       }
            //       int in_channel = channel->GetSinkPort();
            //       _rf(router, f, in_channel, &f->la_route_set, false);
            //     }
            //   }
            //   else
            //   {
            //     f->la_route_set.Clear();
            //   }
            // }

#ifdef TRACK_FLOWS
            ++_outstanding_credits[f->cl][output];
            _outstanding_classes[output][f->vc].push(f->cl);
#endif
            // if(output == 5 && input ==3 && vc == 0 && match_vc == 0)
            //   cout<<GetSimTime()<<" f_dup "<<f_dup->id<<endl;
            dest_buf->SendingFlit(f_dup);
            // if(GetID() == 10 && f_dup->id == 29049)
            // {
            //   cout<<" blech"<<endl;
            // }
            _crossbar_flits.push_back(make_pair(-1, make_pair(f_dup, make_pair(expanded_input, expanded_output))));

            if (mcount == cur_buf->GetMcastTable(vc).size())
            {
                if (_out_queue_credits.count(input) == 0)
                {
                    _out_queue_credits.insert(make_pair(input, Credit::New()));
                }
                _out_queue_credits.find(input)->second->vc.insert(vc);
            }
            if (cur_buf->Empty(vc))
            {
                cur_buf->SetMCastCount(vc, 0);
                assert(mcount == cur_buf->GetMcastTable(vc).size());
                if (f_dup->tail)
                {
                    // cout<<" comes here right"<<endl;
                    if (mcount == cur_buf->GetMcastTable(vc).size())
                    {
                        cur_buf->SetState(vc, VC::idle);
                    }

                    cur_buf->EraseMcastTable(vc);
                    cur_buf->EraseOutpair(vc);
                    if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0))
                    {
                        *gWatchOut << " mcast all output of pid= " << f_dup->pid << " is acquired, fid= " <<
                            f_dup->id << " turn to idle = "
                            << " Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                    }
                }
            }
            else
            {
                Flit* const nf = cur_buf->FrontFlit(vc);
                assert(nf);
                assert(nf->vc == vc);
                if (f_dup->tail) {
                    if (mcount == cur_buf->GetMcastTable(vc).size()) {
                        assert(nf->head);
                        if (_routing_delay)
                        {
                            cur_buf->SetState(vc, VC::routing);
                            if (nf->mflag) {
                                _route_vcs_multi.push_back(make_pair(-1, item.second.first.first));
                            }
                            else
                            {
                                _route_vcs.push_back(make_pair(-1, item.second.first.first));
                            }
                            cur_buf->EraseMcastTable(vc);
                            cur_buf->EraseOutpair(vc);
                            cur_buf->SetMCastCount(vc, 0);
                            if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0))
                            {
                                *gWatchOut << " mcast all output is acquired pid= " << f_dup->pid << " fid= " <<
                                    f_dup->id << " turn to routing for pid = " << nf->pid << " fid = " << nf->id
                                    << " Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                            }
                        }
                    }
                    else if (mcount < cur_buf->GetMcastTable(vc).size()) {
                        /*
                        _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first,
                            -1)));*/
                        if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0))
                        {
                            *gWatchOut << " mcast "<< cur_buf->GetMcastTable(vc).size() - mcount <<" output of pid = " << f_dup->pid << " is not acquired, fid = " 
                                << f_dup->id << " packet_size = "<<f_dup->flits_num << " Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                        }
                    }

                }
                else if (!f_dup->tail) {
                    
                    if (mcount == cur_buf->GetMcastTable(vc).size()) {
                        
                        vector<int> outputandvc = cur_buf->GetMulticastOutpair(vc);
                        // cout<<" my size "<<outputandvc.size()<<endl;
                        for (int i = 0; i < outputandvc.size(); i++)
                        {
                            _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(make_pair(item.second.first.first, outputandvc[i]), -1)));
                        }
                        cur_buf->SetMCastCount(vc, 0);
                        if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0))
                        {
                            *gWatchOut << " mcast all output is acquired pid=" << f_dup->pid << " fid= " << f_dup->id
                                << " packet_size = " << f_dup->flits_num << " Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                        }
                    }
                    else if (mcount < cur_buf->GetMcastTable(vc).size()) {
                        /*
                        _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first,
                            -1)));*/
                        if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0))
                        {
                            *gWatchOut << " mcast " << cur_buf->GetMcastTable(vc).size() - mcount << " output of pid = " << f_dup->pid << " is not acquired, fid = "
                                << f_dup->id << " packet_size = " << f_dup->flits_num << " Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                        }
                    }

                }

                /*
                else
                {
                    if (nf->watch || (_routers_to_watch.count(GetID()) > 0))
                    {
                        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                            << "Using precomputed lookahead routing information for VC " << vc
                            << " at input " << input
                            << " (front: " << nf->id
                            << ")." << endl;
                    }
                    cur_buf->SetRouteSet(vc, &nf->la_route_set);
                    cur_buf->SetState(vc, VC::vc_alloc);
                }*/
                /*
                if (_speculative)
                {
                    if (nf->mflag) {
                        _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first,
                                                                  -1)));
                }
                if (_vc_allocator)
                {
                  _vc_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first,
                                                                  -1)));
                }
                if (_noq)
                {
                  _UpdateNOQ(input, vc, nf);
                }
                */
            }
            /*
            else if (!cur_buf->Empty(vc) && mcount < cur_buf->GetMcastTable(vc).size()) //Body flits
            {
              if (_hold_switch_for_packet)
              {
                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                  *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                             << "Setting up switch hold for VC " << vc
                            << " at input " << input
                             << "." << (expanded_input % _input_speedup)
                             << " to output " << output
                             << "." << (expanded_output % _output_speedup)
                             << "." << endl;
                }
                _switch_hold_vc[expanded_input] = vc;
                _switch_hold_in[expanded_input] = expanded_output;
                _switch_hold_out[expanded_output] = expanded_input;
                _sw_hold_vcs.push_back(make_pair(-1, make_pair(item.second.first,
                                                               -1)));
              }
              //Body flit and tail flit gets pushed back here
                _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first,
                    -1)));
            }

            if (mcount == cur_buf->GetMcastTable(vc).size())
            {
                // cout<<"Im pushing here "<<f->id<<endl;
                cur_buf->SetMCastCount(vc, 0);
                vector<int> outputandvc = cur_buf->GetMulticastOutpair(vc);
                // cout<<" my size "<<outputandvc.size()<<endl;
                for (int i = 0; i < outputandvc.size(); i++)
                {
                    _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(make_pair(item.second.first.first, outputandvc[i]), -1)));
                }
                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                    *gWatchOut << "pushin into switch alloc swalloc update if output got" << f->id << "Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                }
                // cout<<"pushin into switch alloc swallocupdate if output got"<<f_dup->id<<endl;

            }

            // }
          // }


            else
            {
                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                    *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                        << "  No output port allocated." << endl;
                }

    #ifdef TRACK_STALLS
                assert((expanded_output == -1) || // for stalls that are accounted for in VC allocation path
                    (expanded_output == STALL_BUFFER_BUSY) ||
                    (expanded_output == STALL_BUFFER_CONFLICT) ||
                    (expanded_output == STALL_BUFFER_FULL) ||
                    (expanded_output == STALL_BUFFER_RESERVED) ||
                    (expanded_output == STALL_CROSSBAR_CONFLICT));
                if (expanded_output == STALL_BUFFER_BUSY)
                {
                    ++_buffer_busy_stalls[f->cl];
                }
                else if (expanded_output == STALL_BUFFER_CONFLICT)
                {
                    ++_buffer_conflict_stalls[f->cl];
                }
                else if (expanded_output == STALL_BUFFER_FULL)
                {
                    ++_buffer_full_stalls[f->cl];
                }
                else if (expanded_output == STALL_BUFFER_RESERVED)
                {
                    ++_buffer_reserved_stalls[f->cl];
                }
                else if (expanded_output == STALL_CROSSBAR_CONFLICT)
                {
                    ++_crossbar_conflict_stalls[f->cl];
                }
    #endif

                _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first, -1)));
                if (f->watch || (_routers_to_watch.count(GetID()) > 0))
                {
                    *gWatchOut << "pushin into switch alloc swallocupdate if output not got" << f->id << "Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
                }


            }
    */
    // if(f->id == 7451)
    // cout<<"im popiing"<<endl;
        }
        else {
        _sw_alloc_vcs_multi.push_back(make_pair(-1, make_pair(item.second.first, -1)));
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
            *gWatchOut << " push into switch alloc sw_alloc_update if output not got" << f->id << " Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
        }
}
        _sw_alloc_vcs_multi.pop_front();
        if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        {
            *gWatchOut << " after pop front Switch allocation to do is " << _sw_alloc_vcs_multi.size() << endl;
        }
    }
}


Flit * IQRouter::_Generate_Duplicates(Flit *cf , int output , bool generate_dup)
{
  /* 
    generatee duplicate flits using global cur_pid and cur_id
   */
  Flit * f_dup;
  
  int temp_size = _packet_size;
  if(cf->type == Flit::READ_REPLY)
    temp_size = _read_reply_size;
  else if(cf->type == Flit::WRITE_REPLY)
    temp_size = _write_reply_size;
  if(cf->head) {
    if( generate_dup) {
      mcast_map[cf->pid][output] =  make_pair(_cur_pid,_cur_id);
      _cur_pid ++;
      _cur_id += temp_size; 
    }
    else {
      mcast_map[cf->pid][output] =  make_pair(cf->pid,cf->id);
    }
  }

  f_dup = Flit::New();
  f_dup->pid    = mcast_map[cf->pid][output].first;
  bool watch = gWatchOut && (_packets_to_watch.count(f_dup->pid) > 0);
  f_dup->id     = mcast_map[cf->pid][output].second;
  f_dup->watch  = cf->watch | watch| (gWatchOut && (_flits_to_watch.count(f_dup->id) > 0));
  f_dup->subnetwork = cf->subnetwork;
  f_dup->src    = cf->src;
  f_dup->itime = GetSimTime();
  f_dup->ctime  = GetSimTime();
  f_dup->cur_router = GetID();
  f_dup->oid = cf->oid;
  f_dup->record = cf->record;
  f_dup->cl     = cf->cl;
  //DNN specific
  f_dup->nn_type=cf->nn_type;
  f_dup->transfer_id = cf->transfer_id;
  f_dup->end = cf->end;
  f_dup->from_ddr = cf->from_ddr;
  f_dup->to_ddr = cf->to_ddr;
  f_dup->layer_name = cf->layer_name;
  f_dup->size = cf->size;
  f_dup->flits_num = cf->flits_num;
    _total_in_flight_flits[f_dup->cl].insert(make_pair(f_dup->id, f_dup));
  if(cf->record) {
        _measured_in_flight_flits[f_dup->cl].insert(make_pair(f_dup->id, f_dup));
  }

  if(gTrace){
      cout<<"New Flit "<<f_dup->src<<endl;
  }
  f_dup->type = cf->type;

  f_dup->head = cf->head;
  f_dup->dest = cf->dest;

  f_dup->pri = cf->pri;

  f_dup->tail = cf->tail;

  f_dup->vc  = cf->vc;
  f_dup->flits_num = cf->flits_num;
  f_dup->inter_dest = cf->inter_dest;

  f_dup->mflag = cf->mflag;

  if (f_dup->watch || (_routers_to_watch.count(GetID()) > 0)) {
      *gWatchOut << GetSimTime() << " | "
          << FullName() << " | " << " rid = " <<GetID() 
          << " Enqueuing Duplicate flit " << f_dup->id
          << " (packet " << f_dup->pid
          << ") created by original packet " << cf->pid <<" head = "<<f_dup->head<< " tail = " << f_dup->tail << " and fid = "<<cf->id<< " at time " << cf->ctime
          <<" record = "<< f_dup->record
          << " source is " << f_dup->src
          << "." << endl;
  }


  mcast_map[cf->pid][output].second++; //Update map with pid of next flit
  if (!generate_dup) {
      f_dup->hops = cf->hops;
      cf->Free();
  }
  return f_dup;
}

//------------------------------------------------------------------------------
// switch traversal
//------------------------------------------------------------------------------

void IQRouter::_SwitchEvaluate()
{
  for (deque<pair<int, pair<Flit *, pair<int, int> > > >::iterator iter = _crossbar_flits.begin();
       iter != _crossbar_flits.end();
       ++iter)
  {

    int const time = iter->first;
    if (time >= 0)
    {
      break;
    }
    iter->first = GetSimTime() + _crossbar_delay - 1;

    Flit const *const f = iter->second.first;
    assert(f);

    int const expanded_input = iter->second.second.first;
    int const expanded_output = iter->second.second.second;

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " beginning crossbar traversal for flit " << f->id
                 << " from input " << (expanded_input / _input_speedup)
                 << "." << (expanded_input % _input_speedup)
                 << " to output " << (expanded_output / _output_speedup)
                 << "." << (expanded_output % _output_speedup)
                 << "." << endl;
    }
  }
}

void IQRouter::_SwitchUpdate()
{
  while (!_crossbar_flits.empty())
  {

    pair<int, pair<Flit *, pair<int, int> > > const &item = _crossbar_flits.front();

    int const time = item.first;
    if ((time < 0) || (GetSimTime() < time))
    {
      break;
    }
    assert(GetSimTime() == time);

    Flit *const f = item.second.first;
    assert(f);

    int const expanded_input = item.second.second.first;
    int const input = expanded_input / _input_speedup;
    assert((input >= 0) && (input < _inputs));
    int const expanded_output = item.second.second.second;
    int const output = expanded_output / _output_speedup;
    assert((output >= 0) && (output < _outputs));

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " completed crossbar traversal for flit " << f->id
                 << " from input " << input
                 << "." << (expanded_input % _input_speedup)
                 << " to output " << output
                 << "." << (expanded_output % _output_speedup)
                 << "." << endl;
    }
    _switchMonitor->traversal(input, output, f);

    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " buffering flit " << f->id
                 << " at output " << output
                 << "." << endl;
    }
    _output_buffer[output].push(f);
    //the output buffer size isn't precise due to flits in flight
    //but there is a maximum bound based on output speed up and ST traversal
    assert(_output_buffer[output].size() <= (size_t)_output_buffer_size + _crossbar_delay * _output_speedup + (_output_speedup - 1) || _output_buffer_size == -1);
    _crossbar_flits.pop_front();
  }
}

//------------------------------------------------------------------------------
// output queuing
//------------------------------------------------------------------------------

void IQRouter::_OutputQueuing()
{
  for (map<int, Credit *>::const_iterator iter = _out_queue_credits.begin();
       iter != _out_queue_credits.end();
       ++iter)
  {

    int const input = iter->first;
    assert((input >= 0) && (input < _inputs));

    Credit *const c = iter->second;
    assert(c);
    assert(!c->vc.empty());

    _credit_buffer[input].push(c);
  }
  _out_queue_credits.clear();
}

//------------------------------------------------------------------------------
// write outputs
//------------------------------------------------------------------------------

void IQRouter::_SendFlits()
{
  for (int output = 0; output < _outputs; ++output)
  {
    if (!_output_buffer[output].empty())
    {
      Flit *const f = _output_buffer[output].front();
      assert(f);
      _output_buffer[output].pop();

#ifdef TRACK_FLOWS
      ++_sent_flits[f->cl][output];
#endif
      if (f->watch || (_routers_to_watch.count(GetID()) > 0))
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << " Sending flit " << f->id
                   << " to channel at output " << output
                   << "." << endl;
      if (gTrace)
      {
        cout << "Outport " << output << endl
             << "Stop Mark" << endl;
      }
      _output_channels[output]->Send(f);
    }
  }
}

void IQRouter::_SendCredits()
{
  for (int input = 0; input < _inputs; ++input)
  {
    if (!_credit_buffer[input].empty())
    {
      Credit *const c = _credit_buffer[input].front();
      assert(c);
      _credit_buffer[input].pop();
      _input_credits[input]->Send(c);
    }
  }
}

//------------------------------------------------------------------------------
// misc.
//------------------------------------------------------------------------------

void IQRouter::Display(ostream &os) const
{
  for (int input = 0; input < _inputs; ++input)
  {
    _buf[input]->Display(os);
  }
}

int IQRouter::GetUsedCredit(int o) const
{
  assert((o >= 0) && (o < _outputs));
  BufferState const *const dest_buf = _next_buf[o];
  return dest_buf->Occupancy();
}

int IQRouter::GetBufferOccupancy(int i) const
{
  assert(i >= 0 && i < _inputs);
  return _buf[i]->GetOccupancy();
}

#ifdef TRACK_BUFFERS
int IQRouter::GetUsedCreditForClass(int output, int cl) const
{
  assert((output >= 0) && (output < _outputs));
  BufferState const *const dest_buf = _next_buf[output];
  return dest_buf->OccupancyForClass(cl);
}

int IQRouter::GetBufferOccupancyForClass(int input, int cl) const
{
  assert((input >= 0) && (input < _inputs));
  return _buf[input]->GetOccupancyForClass(cl);
}
#endif

vector<int> IQRouter::UsedCredits() const
{
  vector<int> result(_outputs * _vcs);
  for (int o = 0; o < _outputs; ++o)
  {
    for (int v = 0; v < _vcs; ++v)
    {
      result[o * _vcs + v] = _next_buf[o]->OccupancyFor(v);
    }
  }
  return result;
}

vector<int> IQRouter::FreeCredits() const
{
  vector<int> result(_outputs * _vcs);
  for (int o = 0; o < _outputs; ++o)
  {
    for (int v = 0; v < _vcs; ++v)
    {
      result[o * _vcs + v] = _next_buf[o]->AvailableFor(v);
    }
  }
  return result;
}

vector<int> IQRouter::MaxCredits() const
{
  vector<int> result(_outputs * _vcs);
  for (int o = 0; o < _outputs; ++o)
  {
    for (int v = 0; v < _vcs; ++v)
    {
      result[o * _vcs + v] = _next_buf[o]->LimitFor(v);
    }
  }
  return result;
}

void IQRouter::_UpdateNOQ(int input, int vc, Flit const *f)
{
  assert(!_routing_delay);
  assert(f);
  assert(f->vc == vc);
  assert(f->head);
  set<OutputSet::sSetElement> sl = f->la_route_set.GetSet();
  assert(sl.size() == 1);
  int out_port = sl.begin()->output_port;
  const FlitChannel *channel = _output_channels[out_port];
  const Router *router = channel->GetSink();
  if (router)
  {
    int in_channel = channel->GetSinkPort();
    OutputSet nos;
    _rf(router, f, in_channel, &nos, false);
    sl = nos.GetSet();
    assert(sl.size() == 1);
    OutputSet::sSetElement const &se = *sl.begin();
    int next_output_port = se.output_port;
    assert(next_output_port >= 0);
    assert(_noq_next_output_port[input][vc] < 0);
    _noq_next_output_port[input][vc] = next_output_port;
    int next_vc_count = (se.vc_end - se.vc_start + 1) / router->NumOutputs();
    int next_vc_start = se.vc_start + next_output_port * next_vc_count;
    assert(next_vc_start >= 0 && next_vc_start < _vcs);
    assert(_noq_next_vc_start[input][vc] < 0);
    _noq_next_vc_start[input][vc] = next_vc_start;
    int next_vc_end = se.vc_start + (next_output_port + 1) * next_vc_count - 1;
    assert(next_vc_end >= 0 && next_vc_end < _vcs);
    assert(_noq_next_vc_end[input][vc] < 0);
    _noq_next_vc_end[input][vc] = next_vc_end;
    assert(next_vc_start <= next_vc_end);
    if (f->watch || (_routers_to_watch.count(GetID()) > 0))
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << " computing lookahead routing information for flit " << f->id
                 << " (NOQ)." << endl;
    }
  }
}
