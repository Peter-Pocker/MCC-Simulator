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

#ifndef _BUFFER_HPP_
#define _BUFFER_HPP_

#include <vector>

#include "vc.hpp"
#include "flit.hpp"
#include "outputset.hpp"
#include "routefunc.hpp"
#include "config_utils.hpp"
#include "module.hpp"
#include "routers/router.hpp"

class Buffer : public Module {
  
  int _occupancy;
  int _size;

  vector<VC*> _vc;

#ifdef TRACK_BUFFERS
  vector<int> _class_occupancy;
#endif

public:
  //Bransan change to constructor, added is_hub variable
  Buffer( const Configuration& config, int outputs,
	  Module *parent, const string& name, int is_hub =0);
  ~Buffer();

  void addFlitMCastEntry( int vc, int dest, int out_port, bool wflag );
  void AddFlit( int vc, Flit *f );

  inline Flit *RemoveFlit( int vc )
  {
    --_occupancy;
#ifdef TRACK_BUFFERS
    int cl = _vc[vc]->FrontFlit()->cl;
    assert(_class_occupancy[cl] > 0);
    --_class_occupancy[cl];
#endif
    return _vc[vc]->RemoveFlit( );
  }
  
  inline Flit *FrontFlit( int vc ) const
  {
    return _vc[vc]->FrontFlit( );
  }
  
  inline bool Empty( int vc ) const
  {
    return _vc[vc]->Empty( );
  }

  inline bool Full( ) const
  {
    return _occupancy >= _size;
  }

  inline VC::eVCState GetState( int vc ) const
  {
    return _vc[vc]->GetState( );
  }

  inline void SetState( int vc, VC::eVCState s )
  {
    _vc[vc]->SetState(s);
  }

  inline const OutputSet *GetRouteSet( int vc ) const
  {
    return _vc[vc]->GetRouteSet( );
  }
  
  inline const vector<int> GetMulticastOutpair( int vc ) const
  {
    return _vc[vc]->GetMulticastOutpair( );
  }

  inline void SetRouteSet( int vc, OutputSet * output_set )
  {
    _vc[vc]->SetRouteSet(output_set);
  }

  inline void PushMOutputandVC( int vc, int outputandvc)
  {
    _vc[vc]->PushMOutputandVC(outputandvc);
  }

  inline void SetMCastCount( int vc, int count)
  {
    _vc[vc]->SetMCastCount(count);
  }

  inline int GetMCastCount( int vc)
  {
    return _vc[vc]->GetMCastCount();
  }

  inline void EraseMcastTable( int vc )
  {
    _vc[vc]->EraseMcastTable( );
  }

  inline void EraseOutpair( int vc )
  {
    _vc[vc]->EraseOutpair( );
  }


  inline void SetOutput( int vc, int out_port, int out_vc )
  {
    _vc[vc]->SetOutput(out_port, out_vc);
  }


  inline void SetInterDest( int vc , int inter_dest )
  {
    _vc[vc]->SetInterDest(inter_dest);
  }

  inline int GetInterDest( int vc ) const
  {
    return _vc[vc]->GetInterDest( );
  }

  inline map<int, pair<vector<int>, vector<int> > > GetMcastTable( int vc )
  {
    return _vc[vc]->GetMcastTable( );
  }

  inline int GetOutputPort( int vc ) const
  {
    return _vc[vc]->GetOutputPort( );
  }

  inline int GetOutputVC( int vc ) const
  {
    return _vc[vc]->GetOutputVC( );
  }



  inline int GetPriority( int vc ) const
  {
    return _vc[vc]->GetPriority( );
  }

  inline void Route( int vc, tRoutingFunction rf, const Router* router, const Flit* f, int in_channel )
  { 
    _vc[vc]->Route(rf, router, f, in_channel);
  }

  // ==== Debug functions ====

  inline void SetWatch( int vc, bool watch = true )
  {
    _vc[vc]->SetWatch(watch);
  }

  inline bool IsWatched( int vc ) const
  {
    return _vc[vc]->IsWatched( );
  }

  inline int GetOccupancy( ) const
  {
    return _occupancy;
  }

  inline int GetOccupancy( int vc ) const
  {
    return _vc[vc]->GetOccupancy( );
  }

  inline int GetSize( ) const
  {
    return _size;
  }

#ifdef TRACK_BUFFERS
  inline int GetOccupancyForClass(int c) const
  {
    return _class_occupancy[c];
  }
#endif

  void Display( ostream & os = cout ) const;
};

#endif 
