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

#ifndef _FLIT_HPP_
#define _FLIT_HPP_

#include <iostream>
#include <stack>
#include <vector>
#include <utility>

#include "booksim.hpp"
#include "outputset.hpp"

class Flit {

public:


  pair < vector <int> , vector <int> >  mdest;

  bool mflag;

  const static int NUM_FLIT_TYPES = 7;
  enum FlitType { READ_REQUEST  = 0, //5,6 ARE FOR DNN ACCELERATORS
		  READ_REPLY    = 1,
		  WRITE_REQUEST = 2,
		  WRITE_REPLY   = 3,
                  ANY_TYPE      = 4, 
          REQUEST = 5,
          DATA = 6};
  FlitType type;

  int vc;
  int flits_num;
  int nn_type;//5 is request, 6 is data

  int cl;
  int layer_num;
  bool head;
  bool tail;
  
  int  ctime;//create time
  int  itime;//inject time
  int  atime;//arrival time

  int cur_router;
  int  id;
  int  pid;
  int  oid; //Bransan added to calculate transaction time for multicast
  int  transfer_id;
  bool end;//for data, record whether this packet is the end of the whole transfer.
  string layer_name;

  bool record;

  int  src;
  int  dest;

  int size;// record the size of the packet to transfer

  int  pri;
  int  hops;
  bool watch;
  int  subnetwork;

  //Bransan added dropped status
  bool dropped;

  bool from_ddr;
  bool to_ddr;

  int inter_dest;
  
  // intermediate destination (if any)
  mutable int intm;

  // phase in multi-phase algorithms
  mutable int ph;

  // Fields for arbitrary data
  void* data ;

  // Lookahead route info
  OutputSet la_route_set;

  void Reset();

  static Flit * New();
  void Free();
  static void FreeAll();

private:

  Flit();
  ~Flit() {}

  static stack<Flit *> _all;
  static stack<Flit *> _free;

};

ostream& operator<<( ostream& os, const Flit& f );

#endif
