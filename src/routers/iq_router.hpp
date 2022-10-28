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

#ifndef _IQ_ROUTER_HPP_
#define _IQ_ROUTER_HPP_

#include <string>
#include <deque>
#include <queue>
#include <set>
#include <map>
#include <utility>


#include "router.hpp"
#include "../routefunc.hpp"


using namespace std;

class VC;
class Flit;
class Credit;
class Buffer;
class BufferState;
class Allocator;
class SwitchMonitor;
class BufferMonitor;

class IQRouter : public Router {
public:
  int _packet_size;
  int _use_read_reply;
  int _read_reply_size;
  int _write_reply_size;

  int _cur_inter_hub;

  int _vcs;

  bool _vc_busy_when_full;
  bool _vc_prioritize_empty;
  bool _vc_shuffle_requests;

  bool _speculative;
  bool _spec_check_elig;
  bool _spec_check_cred;
  bool _spec_mask_by_reqs;
  
  bool _active;

  int _routing_delay;
  int _vc_alloc_delay;
  int _sw_alloc_delay;
  
  map<int, Flit *> _in_queue_flits;
  map<int, map< int, pair<int,int> > > mcast_map;

  deque<pair<int, pair<Credit *, int> > > _proc_credits;

  //MultiCast Structures
  deque<pair<int, pair<int, int> > > _route_vcs_multi;//pair<time,pair<input,vc>> 
  deque<pair<int, pair<pair<pair<int, int>, int >,int> > > _sw_alloc_vcs_multi;   //pair<time,pair<pair<pair<input,vc>,outputandvc,for switch>
  deque<pair<int, pair<pair<int, int>, pair<bool , int> > > > _vc_alloc_vcs_multi; //pair<time,pair<pair<input,vc>,pair<output,vc>> (each output port demand has an entry)
  deque<pair<int, pair<Flit *, pair<int, int> > > > _crossbar_flits_multi;
  
  deque<pair<int, pair<int, int> > > _route_vcs;                //pair<time,pair<input,vc>> 
  deque<pair<int, pair<pair<int, int>, int> > > _vc_alloc_vcs;  //pair<time,pair<pair<input,vc>,outputandvc=ourput*vcs+vc> 
  deque<pair<int, pair<pair<int, int>, int> > > _sw_hold_vcs;
  deque<pair<int, pair<pair<int, int>, int> > > _sw_alloc_vcs;

  deque<pair<int, pair<Flit *, pair<int, int> > > > _crossbar_flits;

  map<int, Credit *> _out_queue_credits;

  vector<Buffer *> _buf;
  vector<BufferState *> _next_buf;

  Allocator *_vc_allocator;
  Allocator *_sw_allocator;
  Allocator *_spec_sw_allocator;
  
  vector<int> _vc_rr_offset;
  vector<int> _sw_rr_offset;

  tRoutingFunction   _rf;

  int _output_buffer_size;
  vector<queue<Flit *> > _output_buffer;

  vector<queue<Credit *> > _credit_buffer;

  bool _hold_switch_for_packet;
  vector<int> _switch_hold_in;
  vector<int> _switch_hold_out;
  vector<int> _switch_hold_vc;

  set<int> _flits_to_watch;
  set<int> _packets_to_watch;
  set<int> _routers_to_watch;

  bool _noq;
  vector<vector<int> > _noq_next_output_port;
  vector<vector<int> > _noq_next_vc_start;
  vector<vector<int> > _noq_next_vc_end;

#ifdef TRACK_FLOWS
  vector<vector<queue<int> > > _outstanding_classes;
#endif

  Flit* _Generate_Duplicates(Flit *cf ,int output, bool generate_dup );
  bool _ReceiveFlits( );
  bool _ReceiveCredits( );

  virtual void _InternalStep( );

  bool _Internal_Sub_Step(bool);
  bool _Multi_Internal_Sub_Step(bool);

  bool _SWAllocAddReq(int input, int vc, int output);
  bool _SWAllocAddReqMulti(int input, int vc, int output);


  void _InputQueuing( );

  void _RouteEvaluate( );
  void _VCAllocEvaluate( );
  void _SWHoldEvaluate( );
  void _SWAllocEvaluate( );
  void _SwitchEvaluate( );

  void _RouteUpdate( );
  void _VCAllocUpdate( );
  void _SWHoldUpdate( );
  void _SWAllocUpdate( );
  void _SwitchUpdate( );

  //Bransan Added Multi cast
  void _RouteEvaluateMulti( );
  void _VCAllocEvaluateMulti( );
  void _SWHoldEvaluateMulti( );
  void _SWAllocEvaluateMulti( );
  // void _SwitchEvaluateMulti( );

  void _RouteUpdateMulti( );
  void _VCAllocUpdateMulti( );
  void _SWHoldUpdateMulti( );
  void _SWAllocUpdateMulti( );
  // void _SwitchUpdateMulti( );

  void _OutputQueuing( );

  void _SendFlits( );
  void _SendCredits( );
  
  void _UpdateNOQ(int input, int vc, Flit const * f);

  
public:

  // ----------------------------------------
  //
  //   Router Power Modellingyes
  //
  // ----------------------------------------

  SwitchMonitor * _switchMonitor ;
  BufferMonitor * _bufferMonitor ;

  IQRouter( Configuration const & config,
	    Module *parent, string const & name, int id,
	    int inputs, int outputs, int is_hub=0);

  // int _is_hub;
  virtual ~IQRouter( );
  
  virtual void AddOutputChannel(FlitChannel * channel, CreditChannel * backchannel);

  virtual void ReadInputs( );
  virtual void WriteOutputs( );
  
  void Display( ostream & os = cout ) const;

  virtual int GetUsedCredit(int o) const;
  virtual int GetBufferOccupancy(int i) const;

#ifdef TRACK_BUFFERS
  virtual int GetUsedCreditForClass(int output, int cl) const;
  virtual int GetBufferOccupancyForClass(int input, int cl) const;
#endif

  virtual vector<int> UsedCredits() const;
  virtual vector<int> FreeCredits() const;
  virtual vector<int> MaxCredits() const;

  SwitchMonitor const * const GetSwitchMonitor() const {return _switchMonitor;}
  BufferMonitor const * const GetBufferMonitor() const {return _bufferMonitor;}


  //Bransan adding fcfs queue of packet id's
  deque<std::pair<int,Flit *> > _out_queue; 

  void addFlitMCastEntry(int dest, int outport, int input , int vc, int wflag);


};

#endif
