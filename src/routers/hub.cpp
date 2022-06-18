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

 THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
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

#include "hub.hpp"
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
#include "../routers/router.hpp"
#include "../outputset.hpp"
#include "../buffer.hpp"
#include "../buffer_state.hpp"
#include "../arbiters/roundrobin_arb.hpp"
#include "../allocators/allocator.hpp"
#include "../power/switch_monitor.hpp"
#include "../power/buffer_monitor.hpp"

#include "../trafficmanager.hpp"


class TrafficManager;

Hub::Hub( Configuration const & config, Module *parent, 
		    string const & name, int id, int inputs, int outputs, int is_hub)
: IQRouter( config, parent, name, id, inputs, outputs, is_hub )
{

  _rec_queue_size = config.GetInt("received_queue_size");
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

  // Routing
  string const rf = config.GetStr("routing_function") + "_" + config.GetStr("topology");
  map<string, tRoutingFunction>::const_iterator rf_iter = gRoutingFunctionMap.find(rf);
  if (rf_iter == gRoutingFunctionMap.end())
  {
    Error("Invalid routing function: " + rf);
  }
  _rf = rf_iter->second;

  // Alloc VC's
  //Hardcoded to 1 since connection to single router
  _buf.resize(1);

  ostringstream module_name;
  module_name << "buf_" << 0;
  _buf[0] = new Buffer(config, _outputs, this, module_name.str(),1);

  
  module_name.str("");


  // Alloc next VCs' buffer state
  _next_buf.resize(_outputs);
  for (int j = 0; j < _outputs; ++j)
  {
    ostringstream module_name;
    module_name << "next_vc_o" << j;
    _next_buf[j] = new BufferState(config, this, module_name.str());
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
  _output_buffer.resize(1); // To the router
  _output_buffer_wireless.resize(_outputs - 1); //To every other hub
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

void Hub::AddOutputChannel(FlitChannel *channel, CreditChannel *backchannel)
{
  int alloc_delay = _speculative ? max(_vc_alloc_delay, _sw_alloc_delay) : (_vc_alloc_delay + _sw_alloc_delay);
  int min_latency = 1 + _crossbar_delay + channel->GetLatency() + _routing_delay + alloc_delay + backchannel->GetLatency() + _credit_delay;
  // cout<<"SIZE "<<_output_channels.size()<<endl;
  _next_buf[_output_channels.size()]->SetMinLatency(min_latency);
  Router::AddOutputChannel(channel, backchannel);
}

void Hub::AddOutputChannel(PayloadChannel *channel, CreditChannel *backchannel)
{
  int alloc_delay = _speculative ? max(_vc_alloc_delay, _sw_alloc_delay) : (_vc_alloc_delay + _sw_alloc_delay);
  int min_latency = 1 + _crossbar_delay + channel->GetLatency() + _routing_delay + alloc_delay + backchannel->GetLatency() + _credit_delay;
  // cout<<"SIZE "<<_output_channels.size()<<endl;
  _next_buf[_payload_out_channels.size()]->SetMinLatency(min_latency);
  Router::AddOutputChannel(channel, backchannel);
}

void Hub::ReadInputs()
{
  bool have_flits = _ReceiveFlits();
  bool have_credits = _ReceiveCredits();
  _active = _active || have_flits || have_credits;
}

void Hub::_InternalStep()
{
  if (!_active)
  {
    return;
  }
  
  _InputQueuing();

  bool activity = !_proc_credits.empty();


  if (_vc_allocator)
  {
    _vc_allocator->Clear();
    if(!_receive_queue.empty())
    {
      _VCAllocEvaluateNew();
    }
  }


  if (!_crossbar_flits.empty())
    _SwitchEvaluate();

  /*
    _SwitchtraverseNew is used to check if the router's vc is not full
    especially when non head flits are sent
  */
  if (!_switch_queue.empty()){
    _SwitchTraverseNew();
     activity = activity || !_switch_queue.empty();
  }

  /*
    HubTraversal starts broadcasting flit by flit 
    given that the token is available , ready token is set
    and the receiving hub as space in the recieve queue
  */
  if(!_hub_traversal.empty() )
  {
    // assert(token_ring[GetID()] && ready_send[GetID()]);
    _HubTraverseNew();
    activity = activity || !_hub_traversal.empty();    
  }


  
  if (!_crossbar_flits.empty())
  {
    _SwitchUpdate();
    activity = activity || !_crossbar_flits.empty();
  }


  //Bransan added this second condition to make sure that out_queue is empty before setting inactive 
  _active = activity || !_out_queue.empty() || !_receive_queue.empty();

  _OutputQueuing();

  _bufferMonitor->cycle();
  _switchMonitor->cycle();
}

void Hub::WriteOutputs()
{
  _SendFlits();
  _SendCredits();
}

//------------------------------------------------------------------------------
// read inputs
//------------------------------------------------------------------------------

bool Hub::_ReceiveFlits()
{
  bool activity = false;
  //Receive from router 
  int input = 0;
  Flit *const f = _input_channels[input]->Receive();
  if (f)
  {

#ifdef TRACK_FLOWS
    ++_received_flits[f->cl][input];
#endif

    if (f->watch)
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                  << "Received flit " << f->id
                  << " from channel at input " << input
                  << "." << endl;
    }
    _in_queue_flits.push_back(make_pair(input, f));
    activity = true;
  }

  //Receive from all other hubs
  //Label : Unpacking of Payload
  
  for (int input = 0; input < _inputs - 1; ++input)
  {
    Payload *const p = _payload_in_channels[input]->Receive();
    if (p)
    {

#ifdef TRACK_FLOWS
      ++_received_flits[f->cl][input];
#endif
      for(int i = 0; i < p->_flits.size(); i++)
      {
        Flit * f = p->_flits[i];
        if (f->watch)
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                    << "Received flit " << f->id
                    << " from channel at input " << input + 1
                    << "." << endl;
        }
        _in_queue_flits.push_back(make_pair(input + 1, f));
        activity = true;
      }
    }
  }

  //End of Label : Unpacking of Payload
  

  return activity;
}

bool Hub::_ReceiveCredits()
{
  bool activity = false;
  //Bransan changed to only 0
  int output = 0;
  Credit *const c = _output_credits[output]->Receive();
  if (c)
  {
    _proc_credits.push_back(make_pair(GetSimTime() + _credit_delay,
                                      make_pair(c, output)));
    activity = true;
  }
  return activity;
}

//------------------------------------------------------------------------------
// input queuing
//------------------------------------------------------------------------------
bool Hub::_ReceiveStatus(int inter_dest){
  int temp_size = _packet_size;
  int flit_type = _out_queue.front().second->type;
  if(flit_type == Flit::READ_REPLY)
  {
    temp_size = _read_reply_size;
  }
  else if(flit_type == Flit::WRITE_REPLY)
  {
    temp_size = _write_reply_size;
  }
  if((ready_receive[inter_dest] - temp_size) < 0 ){
    return false;
  }
  return true;
}

bool Hub::_ReceiveStatus( )
{
  int temp_size = _packet_size;
  int flit_type = _out_queue.front().second->type;
  if(flit_type == Flit::READ_REPLY)
  {
    temp_size = _read_reply_size;
  }
  else if(flit_type == Flit::WRITE_REPLY)
  {
    temp_size = _write_reply_size;
  }
  for(int i = 0; i< ready_receive.size() ;i++)
  {
    if(i != GetID())
    {
      if((ready_receive[i] - temp_size) < 0 )
      {
        return false;
      }
    }
  }
  return true;
}


void Hub::_UpdateReceiveStatus(int inter_dest)
{
  assert(ready_receive[inter_dest]);
  ready_receive[inter_dest]--;
}

void Hub::_UpdateReceiveStatusMulti( )
{
  // assert(ready_receive[inter_dest]);
  for(int i = 0; i< ready_receive.size() ;i++)
  {
    if(i != GetID())
    {
      assert(ready_receive[i]);
      ready_receive[i]--;
    }
  }
  
}

void Hub::_dest_reducto(Flit * f)
{
  vector <int> dests = f->mdest.second;
  assert(f->mdest.second.size());
  for ( vector <int>::iterator i = dests.end() - 1; i >= dests.begin() ; i--)
  {
    if(hub_mapper[*i].second != GetID())
      dests.erase(i);
  }
  f->mdest.first = dests;
  f->mdest.second.clear();
}

void Hub::_InputQueuing()
{
  for (vector<pair<int, Flit *> >::const_iterator iter = _in_queue_flits.begin();
       iter != _in_queue_flits.end();
       ++iter)
  {
    int const input = iter->first;
    assert((input >= 0) && (input < _inputs));

    Flit *const f = iter->second;
    
    assert(f);
    /* 
      if input = 0 add flit to vc
      store flits until tail arrives
      then sent ready_send to true
    */
    if(input == 0)
    {

      int const vc = f->vc;
      assert((vc >= 0) && (vc < _vcs));

      Buffer *const cur_buf = _buf[input];

      if (f->watch)
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                  << "Adding flit " << f->id
                  << " to VC " << vc
                  << " at input " << input
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
        *gWatchOut << ")." << endl;
        
      }
      cur_buf->AddFlit(vc, f);
      
      if(f->tail){
        hwait_clock[GetID()].insert(make_pair(f->pid,GetSimTime()));
      }

      #ifdef TRACK_FLOWS
          ++_stored_flits[f->cl][input];
          if (f->head)
            ++_active_packets[f->cl][input];
      #endif

      _bufferMonitor->write(input, f);

      
      //Label : Ready_Send Update

      if(f->tail)
      {
        //Bransan update ready status
        
        _out_queue.push_back(make_pair(iter->first,iter->second));
        ready_send[GetID()] = true;
        if (f->watch)
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
          <<"Received tail at hub "<<GetID() <<" hub ready status : "<< ready_send[GetID()]
          <<" bcast outqueue front flit "<<_out_queue.front().second->id
          <<" remaining "<<_out_queue.size()-1
          << endl;
        }
      }
      
      //End of Label : Ready_Send Update
      
    }
    else  //Input non zero
    {
      if(f->mflag)
      {
        _dest_reducto(f);
      }
      /* 
        if input != 0 add flit to receive queue
        drop unnecessary flits
      */

      if( (f->head && f->inter_dest != GetID() && !f->mflag) || (f->mflag && f->mdest.first.empty()) )
      {
        if(f->mflag)
        {
            ready_receive[GetID()]++;
            f->dropped = true;
           // ready_receive[GetID()]++;
            _dropped_flits.push_back(f);  // this flit is dropped as it came to wrong hub
            continue;
        }
        f->dropped = true;
        // ready_receive[GetID()]++;
        _dropped_flits.push_back(f);  // this flit is dropped as it came to wrong hub
        _drop_set.insert(f->pid);
        continue;
      }
      else
      {
        if( !f->head && _drop_set.find(f->pid) != _drop_set.end())
        {
            f->dropped = true;
            // ready_receive[GetID()]++;
            _dropped_flits.push_back(f);  // this flit is dropped as it came to wrong hub
            if(f->tail)
              _drop_set.erase(f->pid);
            continue;
        }
        // cout<<"rec_queue_size "<<_receive_queue.size()<<" fid "<<f->id<<" hubid "<<GetID()<<endl;
        // if(GetID() == 3)
        //   cout<<"ready receive : "<<ready_receive[GetID()]<<endl;
        assert((int)_receive_queue.size() < _rec_queue_size);
        assert(f);
        
        _receive_queue.push_back(make_pair(make_pair(input,f),-1));
        // if(GetSimTime()>961 && GetID() == 0)
        // {
        //   cout<<"re queue size "<<_receive_queue.size()<<" cur flit "<<f->id<<" front flit "<<_receive_queue.front().first.second->id<<endl;
        // }
        
        if(f->watch)
        {  
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                    << "Adding flit " << f->id
                    << "to receive queue"<<endl;
        }
      }
      
    }
    
  } 
  _in_queue_flits.clear();

  /*
    if hub is ready to send 
    pop from outqueue
    and push into hub traversal
  */

  //Label : Ready to Send Condition

  bool A = token_ring[GetID()];
  bool B = ready_send[GetID()];
  bool C;

  if(!_out_queue.empty())
  {
    if(_out_queue.front().second->mflag)
    {
      C = _ReceiveStatus( );    
    }
    else
    {
      C = _ReceiveStatus(_out_queue.front().second->inter_dest);    
    }
  }
  else
  {
    C = 0;
  }
  
  stats[GetID()][(int)(A*4) + (int)(B*2) + (int)C]++;
  if(A && B && C && (token_hold == false))
  {
    
    token_hold = true;
    pair<int, Flit *> temp_pair = _out_queue.front();
    htime_and_cnt[GetID()].first += GetSimTime() - hwait_clock[GetID()][temp_pair.second->pid];
    htime_and_cnt[GetID()].second++;
    assert(_routing_delay);
    assert(temp_pair.first == 0);
    Buffer * cur_buf = _buf[0];
    assert(cur_buf->GetState(temp_pair.second->vc) == VC::idle);
    // if(_out_queue.front().second->id == 6203)
    //   cout<<"entered"<<endl;
    // cout<<"Flit id "<<temp_pair.second->id<<" Flit source "<<temp_pair.second->src<<" Flit dest "<<temp_pair.second->dest<<endl;
    cur_buf->SetState(temp_pair.second->vc,  VC::active);
    // _route_vcs.push_back(make_pair(-1, make_pair(0, temp_pair.second->vc)));
    if(_hub_traversal.empty())
    {
      // cout<<GetSimTime()<<"Pushed into hubtraversal "<<GetID()<<" for flit "<<_out_queue.front().second->id<<" space left "<<ready_receive[2]<<endl;
      _hub_traversal.push_back(make_pair(GetSimTime(), temp_pair.second->vc));
    }
    // cout<<"Hub traversal "<<_hub_traversal.size()<<endl;
  }
  else
  {
    if(!_out_queue.empty())
    {
      Flit *f = _out_queue.front().second;
      if(f->watch)
      {  
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
        << " Conditions failed : Token " << A
        << " Ready to Send "<<B
        << " Ready to receive "<<C
        << " space left in interdest "<<ready_receive[f->inter_dest]
        <<endl;
      }

    }
  }
  

  //End of Label : Ready to Send Condition


  while (!_proc_credits.empty())
  {
    // if(GetID() == 2)
    //   cout<<"Do i enter"<<endl;

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
    // if(GetSimTime()>=999)
    // cout<<"just error hub\n";
    dest_buf->ProcessCredit(c);
    c->Free();
    _proc_credits.pop_front();
  }
}



void Hub::_VCAllocEvaluateNew()
{
  /*
  * perform vc allocation only for the front of the receive queue
  * only if its not a head
  * else directly push for swithc traversal
  */
  assert(_vc_allocator);

  bool watched = false;
  Flit * f = _receive_queue.front().first.second;
  int input = _receive_queue.front().first.first;
  if(!f->head) {
    _receive_queue.front().second = _cur_out_vc;
    return;
  }
  

    if (f->watch)
    {
      // cout<<"TIme is "<<time<<endl;
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << "Beginning VC allocation for receive queue flit"
                 << " (front: " << f->id
                 << ")." << endl;
    }

    // OutputSet const *const route_set = cur_buf->GetRouteSet(vc);
    // assert(route_set);

    int const out_priority = f->pri;
    // set<OutputSet::sSetElement> const setlist = route_set->GetSet();

    bool elig = false;
    bool cred = false;
    bool reserved = false;

    int const out_port = 0; //to the router
    // assert((out_port >= 0) && (out_port < _outputs));

    BufferState const *const dest_buf = _next_buf[out_port];

    int vc_start;
    int vc_end;

    //This is needed to implement the read reply model
    int vcBegin = 0, vcEnd = gNumVCs-1;
    if ( f->type == Flit::READ_REQUEST ) {
      vcBegin = gReadReqBeginVC;
      vcEnd = gReadReqEndVC;
    } else if ( f->type == Flit::WRITE_REQUEST ) {
      vcBegin = gWriteReqBeginVC;
      vcEnd = gWriteReqEndVC;
    } else if ( f->type ==  Flit::READ_REPLY ) {
      vcBegin = gReadReplyBeginVC;
      vcEnd = gReadReplyEndVC;
    } else if ( f->type ==  Flit::WRITE_REPLY ) {
      vcBegin = gWriteReplyBeginVC;
      vcEnd = gWriteReplyEndVC;
    }
    vc_start = vcBegin;
    vc_end = vcEnd;
    


    for (int out_vc = vc_start; out_vc <= vc_end; ++out_vc)
    {
      // assert((out_vc >= 0) && (out_vc < _vcs));

      int in_priority = 0;
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
        if (f->watch)
        {
          int const use_input_and_vc = dest_buf->UsedBy(out_vc);
          int const use_input = use_input_and_vc / _vcs;
          int const use_vc = use_input_and_vc % _vcs;
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                      << "  VC " << out_vc
                      << " at output " << out_port
                      << " is in use by VC " << use_vc
                      << " at input " << use_input;
         
          *gWatchOut << "." << endl;
        }
      }
      else
      {
        elig = true;
        if (_vc_busy_when_full && dest_buf->IsFullFor(out_vc))
        {
          if (f->watch)
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                        << "  VC " << out_vc
                        << " at output " << out_port
                        << " is full." << endl;
          reserved |= !dest_buf->IsFull();
        }
        else
        {
          cred = true;
          if (f->watch)
          {
            *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                        << "  Requesting VC " << out_vc
                        << " at output " << out_port
                      //  << " (in_pri: " << in_priority
                      //  << ", out_pri: " << out_priority
                        << ")." << endl;
            watched = true;
          }
          int const input_and_vc = _vc_shuffle_requests ? (0 * _inputs + input) : (input * _vcs + 0);
          _vc_allocator->AddRequest(input_and_vc, out_port * _vcs + out_vc,
                                    0, in_priority, out_priority);
        }
      }
    }
  // }
  if (!elig)
  {
    _receive_queue.front().second = STALL_BUFFER_BUSY;
  }
  else if (_vc_busy_when_full && !cred)
  {
    _receive_queue.front().second = reserved ? STALL_BUFFER_RESERVED : STALL_BUFFER_FULL;
  }
  

  if (watched)
  {
    *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | " << " rid = " <<GetID() ;
    _vc_allocator->PrintRequests(gWatchOut);
  }

  _vc_allocator->Allocate();

  if (watched)
  {
    *gWatchOut << GetSimTime() << " | " << _vc_allocator->FullName() << " | " << " rid = " <<GetID() ;
    _vc_allocator->PrintGrants(gWatchOut);
  }

  

    int const input_and_vc = _vc_shuffle_requests ? (0 * _inputs + input) : (input * _vcs + 0);
    // int const input_and_vc = _vc_shuffle_requests ? (vc * _inputs + input) : (input * _vcs + vc);
    int const output_and_vc = _vc_allocator->OutputAssigned(input_and_vc);

    if (output_and_vc >= 0)
    {

      int const match_output = output_and_vc / _vcs;
      assert((match_output >= 0) && (match_output < _outputs));
      int const match_vc = output_and_vc % _vcs;
      assert((match_vc >= 0) && (match_vc < _vcs));

      if (f->watch)
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "Assigning VC " << match_vc
                   << " at output " << match_output
                   << " at input " << input
                   << "." << endl;
      }

      _receive_queue.front().second = output_and_vc;
    }
    else
    {

      if (f->watch)
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "VC allocation failed for recieve queue at input " << input
                   << "." << endl;
      }

       _receive_queue.front().second = STALL_BUFFER_CONFLICT;
    }
  

    int const output_vc = _receive_queue.front().second ;

    if (output_vc >= 0)
    {

      int const match_output = output_vc / _vcs;
      assert((match_output >= 0) && (match_output < _outputs));
      int const match_vc = output_vc % _vcs;
      assert((match_vc >= 0) && (match_vc < _vcs));

      BufferState const *const dest_buf = _next_buf[match_output];

     

      if (!dest_buf->IsAvailableFor(match_vc))
      {
        if (f->watch)
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  Discarding previously generated grant for "
                     << " at input " << input
                     << ": VC " << match_vc
                     << " at output " << match_output
                     << " is no longer available." << endl;
        }
        _receive_queue.front().second = STALL_BUFFER_BUSY;
      }
      else if (_vc_busy_when_full && dest_buf->IsFullFor(match_vc))
      {
        if (f->watch)
        {
          *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                     << "  Discarding previously generated grant "
                     << " at input " << input
                     << ": VC " << match_vc
                     << " at output " << match_output
                     << " has become full." << endl;
        }
        _receive_queue.front().second = dest_buf->IsFull() ? STALL_BUFFER_FULL : STALL_BUFFER_RESERVED;
      }
    }
  // }
  _cur_out_vc = _receive_queue.front().second;
  int const match_vc = _cur_out_vc;
  BufferState *const dests_buf = _next_buf[0]; //0 is outport
  
  if(_cur_out_vc >= 0){ //VC assigned
    // cout<<"Hub id "<<GetID()<<endl;
    dests_buf->TakeBuffer(match_vc, input * _vcs + 0); 
    if (f->watch)
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "  Acquiring assigned VC " << match_vc
                   << " at output " << 0
                   << "." << endl;
      }
      _switch_queue.push_back(make_pair(make_pair(input,f),match_vc));
      // cout<<"receive_qu size second "<<_receive_queue.size()<<endl;
      
  }

  
}


vector <Flit *> Hub::_Generate_Duplicates(Flit *cf )
{
  /* 
    generatee duplicate flits using global cur_pid and cur_id
   */
  vector <Flit *> f_list;
  f_list.resize(_outputs - 1);
  f_list[_outputs - 2] = cf;
  int temp_size = _packet_size;
  if(cf->type == Flit::READ_REPLY)
    temp_size = _read_reply_size;
  else if(cf->type == Flit::WRITE_REPLY)
    temp_size = _write_reply_size;

  if(cf->head) {
    bcast_map[cf->pid] =  make_pair(_cur_pid,_cur_id);
    _cur_pid += (_outputs - 2);
    _cur_id += (_outputs - 2) * temp_size;
  }

  for(int i = 0 ; i < (_outputs - 2) ; i++){
    f_list[i] = Flit::New();
    f_list[i]->pid    = bcast_map[cf->pid].first + i;
    f_list[i]->id     = bcast_map[cf->pid].second + (i * temp_size);
    f_list[i]->oid    = cf->oid;
    // assert( _cur_id);
    f_list[i]->watch  = cf->watch;
    f_list[i]->subnetwork = cf->subnetwork;
    f_list[i]->src    = cf->src;
    f_list[i]->ctime  = cf->ctime;
    f_list[i]->itime  = cf->itime;
    f_list[i]->record = cf->record;
    f_list[i]->cl     = cf->cl;
    f_list[i]->inter_dest = cf->inter_dest;

     _total_in_flight_flits[f_list[i]->cl].insert(make_pair(f_list[i]->id, f_list[i]));
    if(cf->record) {
         _measured_in_flight_flits[f_list[i]->cl].insert(make_pair(f_list[i]->id, f_list[i]));
    }

    if(gTrace){
        cout<<"New Flit "<<f_list[i]->src<<endl;
    }
    f_list[i]->type = cf->type;

        f_list[i]->head = cf->head;
        f_list[i]->dest = cf->dest;
    
        f_list[i]->pri = cf->pri;
      
        f_list[i]->tail = cf->tail;

    f_list[i]->vc  = cf->vc;
    f_list[i]->mflag = cf->mflag;
    f_list[i]->mdest = cf->mdest;
    

    if ( f_list[i]->watch ) { 
        *gWatchOut << GetSimTime() << " | "
                    << "node" << cf->src << " | "
                    << "Enqueuing Duplicate flit " << f_list[i]->id
                    << " (packet " << f_list[i]->pid
                    << ") at time " << cf->ctime
                    << "." << endl;
    }
    // if(f_list[i]->id == 2367)
    // {
    //   cout<<"orig "<<cf->id<<endl;
    // }
  } 
  bcast_map[cf->pid].second++; //Update map with pid of next flit
  return f_list;
}

void Hub::_HubTraverseNew()
{
  
  /*   
    set ready status to false if tail arrives and outqueue is empty
    pass the token after tail is sent
    also check if recieve queue has space
   */

    pair <int, int> item = _hub_traversal.front();

    int time = item.first;
    if(time == GetSimTime()){
      return;
    }


    vector<Payload *> payload;
    payload.resize(_outputs - 1);

    //Label : Payload Creation

    for(int i=0;i < payload.size() ; i++)
      payload[i] = new Payload();

    //End of Label : Payload Creation

    int input = 0;
    int vc = item.second;   
    assert((vc >= 0) && (vc < _vcs));
    Buffer *const cur_buf = _buf[input];
    assert(!cur_buf->Empty(vc));
    assert((cur_buf->GetState(vc) == VC::active));

    while(!cur_buf->Empty(vc)){

      Flit *const f = cur_buf->FrontFlit(vc);
      assert(f);
      assert(f->vc == vc);
      if (f->watch)
      {
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                  << "Completed Hub Traversal for VC " << vc
                  << " at input " << input
                  << " (front: " << f->id
                  << ")." << endl;
      }
      cur_buf->RemoveFlit(vc);

      //Label : Ready_Send Update
      if(f->tail)
      {
        
        cur_buf->SetState(vc, VC::idle);
        _out_queue.pop_front();
        if(_out_queue.empty())
        {
          ready_send[GetID()] = false;
        }

        token_hold=false;

        if (f->watch)
        {
          *gWatchOut <<"Send out tail at hub "<<GetID() <<" hub ready status : "<< ready_send[GetID()]<< endl;
        }
        
        cur_buf->SetState(vc, VC::idle);
      }

      //End of Label : Ready_Send Update

      

      //Do I even need this?
      _bufferMonitor->read(input, f);
      f->hops++;
      f->vc = 0;
      
      // int expanded_input = input * _input_speedup + vc % _input_speedup;
      vector <Flit *> bcast_flits = _Generate_Duplicates(f); 
      for(unsigned int i = 0 ; i < bcast_flits.size() ; i++ ){
        // cout<<"psize "<<payload.size()<<""
        //  _crossbar_flits.push_back(make_pair(-1, make_pair(bcast_flits[i], make_pair(expanded_input, (i+1)*_output_speedup))));
        payload[i]->AddFlits(bcast_flits[i]);
        if(bcast_flits[i]->watch)
        {
          payload[i]->watch = bcast_flits[i]->watch;
          payload[i]->id = bcast_flits[i]->id;
        }
      }


      //Label : Ready_Receive Update
      if(f->mflag)
      {
        _UpdateReceiveStatusMulti();
      }
      else
      {
        _UpdateReceiveStatus(f->inter_dest);
      }
      

      //End of Label : Ready_Receive Update

      
      if(f->id == 6203)
        cout<<"Pushed entry "<<endl;
      _out_queue_credits.push_back(make_pair(input, Credit::New()));

      for( int i = _out_queue_credits.size() - 1; i >=0 ; i-- )
      {
        if(_out_queue_credits[i].first == input)
        {
           _out_queue_credits[i].second->vc.insert(vc);
          break;
        }
      }

      //_out_queue_credits.find(input)->second->vc.insert(vc);

    }

    

    for(unsigned int i = 0 ; i < payload.size() ; i++ ){
        _output_buffer_wireless[i].push(payload[i]);
    }

    
    _hub_traversal.pop_front();


}

//------------------------------------------------------------------------------
// switch traversal
//------------------------------------------------------------------------------

void Hub::_SwitchTraverseNew ()
{
  /* 
  check for destination 
    vc 
    space
   */
  Flit * f = _switch_queue.front().first.second;
  int match_vc = _switch_queue.front().second;
  int input = _switch_queue.front().first.first;
  _bufferMonitor->read(input, f);


  int const expanded_input = input * _input_speedup ;
  int const expanded_output = 0 * _input_speedup ;
  BufferState *const dest_buf = _next_buf[0];
  if(dest_buf->IsFullFor(match_vc)){
    if(f->watch)
    {
      *gWatchOut<<"Hub ID: "<<GetID()
      <<" Buffer full at vc "<<match_vc
      <<" Flit "<<f->id
      <<" is stalled in receive queue of Hub"
      <<endl;
    }
    // cout<<match_vc<<" its full\n";
    return;
  }
  // else{
  //   // cout<<"I got me some buffer "<<match_vc<<endl;
  // }

  f->hops++;
  f->vc = match_vc;
  
  dest_buf->SendingFlit(f);
  
  _crossbar_flits.push_back(make_pair(-1, make_pair(f, make_pair(expanded_input, expanded_output))));
  
  // if(GetID() == 0 && !_receive_queue.empty())
  //   cout<<GetSimTime()<<" Before Receive queue front "<<_receive_queue.front().first.second->id<<" Size "<<_receive_queue.size()<<endl;
  _receive_queue.pop_front();
  // if(GetID() == 0 && !_receive_queue.empty())
  //   cout<<GetSimTime()<<" After pop Receive queue front "<<_receive_queue.front().first.second->id<<" Size "<<_receive_queue.size()<<endl;
  
  assert((int)_receive_queue.size() < _rec_queue_size);
  
  //Label : Ready_Receive Update
  
  ready_receive[GetID()]++; // Freed more space in the receive queue;
  //End of Label : Ready_Receive Update
  
  
  _switch_queue.pop_front(); //Eliminate switch queue when the time is right

  
  if(!_receive_queue.empty())  //Making an entry for the next flit it the packet
  {
    Flit *nf = _receive_queue.front().first.second;  
    assert(nf);
    if(!nf->head){
      _switch_queue.push_back(make_pair(make_pair(input,nf),match_vc));
      // cout<<"receive_qu size third "<<_receive_queue.size()<<endl;

    }
  }

}
void Hub::_SwitchEvaluate()
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

    if (f->watch)
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << "Beginning crossbar traversal for flit " << f->id
                 << " from input " << (expanded_input / _input_speedup)
                 << "." << (expanded_input % _input_speedup)
                 << " to output " << (expanded_output / _output_speedup)
                 << "." << (expanded_output % _output_speedup)
                 << "." << endl;
    }
  }
}

void Hub::_SwitchUpdate()
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

    if (f->watch)
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << "Completed crossbar traversal for flit " << f->id
                 << " from input " << input
                 << "." << (expanded_input % _input_speedup)
                 << " to output " << output
                 << "." << (expanded_output % _output_speedup)
                 << "." << endl;
    }
    _switchMonitor->traversal(input, output, f);

    if (f->watch)
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << "Buffering flit " << f->id
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

void Hub::_OutputQueuing()
{
  for (vector<pair<int, Credit *> >::const_iterator iter = _out_queue_credits.begin();
       iter != _out_queue_credits.end();
       ++iter)
  {
    // cout<<"Am i here?"<<endl;

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

void Hub::_SendFlits()
{
  int output = 0;
  if (!_output_buffer[output].empty())
  {
    Flit *const f = _output_buffer[output].front();
    assert(f);
    _output_buffer[output].pop();

#ifdef TRACK_FLOWS
    ++_sent_flits[f->cl][output];
#endif
    if (f->watch)
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                  << "Sending flit " << f->id
                  << " to channel at output " << output
                  << "." << endl;
    if (gTrace)
    {
      cout << "Outport " << output << endl
            << "Stop Mark" << endl;
    }
    _output_channels[output]->Send(f);
  }

  for (int output = 0; output < _outputs-1; ++output)
  {
    if (!_output_buffer_wireless[output].empty())
    {
      Payload *const p = _output_buffer_wireless[output].front();
      assert(p);
      _output_buffer_wireless[output].pop();

#ifdef TRACK_FLOWS
      ++_sent_flits[p->cl][output];
#endif
      if (p->watch)
        *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                   << "Sending flit " << p->id
                   << " to channel at output " << output + 1
                   << "." << endl;
      if (gTrace)
      {
        cout << "Outport " << output << endl
             << "Stop Mark" << endl;
      }
      _payload_out_channels[output]->Send(p);
    }
  }

}

void Hub::_SendCredits()
{
  int input = 0;
  if (!_credit_buffer[input].empty())
  {
    // if(GetID() == 2 && input == 0)
    //   cout<<"asfjj"<<endl;
    Credit *const c = _credit_buffer[input].front();
    assert(c);
    _credit_buffer[input].pop();
    _input_credits[input]->Send(c);
  }
}

//------------------------------------------------------------------------------
// misc.
//------------------------------------------------------------------------------

void Hub::Display(ostream &os) const
{
  for (int input = 0; input < _inputs; ++input)
  {
    _buf[input]->Display(os);
  }
}

int Hub::GetUsedCredit(int o) const
{
  assert((o >= 0) && (o < _outputs));
  BufferState const *const dest_buf = _next_buf[o];
  return dest_buf->Occupancy();
}

int Hub::GetBufferOccupancy(int i) const
{
  assert(i >= 0 && i < _inputs);
  return _buf[i]->GetOccupancy();
}

#ifdef TRACK_BUFFERS
int Hub::GetUsedCreditForClass(int output, int cl) const
{
  assert((output >= 0) && (output < _outputs));
  BufferState const *const dest_buf = _next_buf[output];
  return dest_buf->OccupancyForClass(cl);
}

int Hub::GetBufferOccupancyForClass(int input, int cl) const
{
  assert((input >= 0) && (input < _inputs));
  return _buf[input]->GetOccupancyForClass(cl);
}
#endif

vector<int> Hub::UsedCredits() const
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

vector<int> Hub::FreeCredits() const
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

vector<int> Hub::MaxCredits() const
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

void Hub::_UpdateNOQ(int input, int vc, Flit const *f)
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
    if (f->watch)
    {
      *gWatchOut << GetSimTime() << " | " << FullName() << " | " << " rid = " <<GetID() 
                 << "Computing lookahead routing information for flit " << f->id
                 << " (NOQ)." << endl;
    }
  }
}
