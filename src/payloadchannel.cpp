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

// ----------------------------------------------------------------------
//
//  File Name: payloadchannel.cpp
//  Author: James Balfour, Rebecca Schultz
//
// ----------------------------------------------------------------------

#include "payloadchannel.hpp"

#include <iostream>
#include <iomanip>

#include "routers/router.hpp"
#include "globals.hpp"

// ----------------------------------------------------------------------
//  $Author: Bransan
//  $Date: 2007/06/27 23:10:17 $
//  $Id$
// ----------------------------------------------------------------------
PayloadChannel::PayloadChannel(Module * parent, string const & name, int classes)
: Channel<Payload>(parent, name), _routerSource(NULL), _routerSourcePort(-1), 
  _routerSink(NULL), _routerSinkPort(-1), _idle(0) {
  _active.resize(classes, 0);
}

void PayloadChannel::SetSource(Router const * const router, int port) {
  _routerSource = router;
  _routerSourcePort = port;
}

void PayloadChannel::SetSink(Router const * const router, int port) {
  _routerSink = router;
  _routerSinkPort = port;
}

void PayloadChannel::Send(Payload * p) {
  if(p) {
    _active[p->_flits[0]->cl] += p->_flits.size();
  } else {
    ++_idle;
  }
  Channel<Payload>::Send(p);
}

void PayloadChannel::ReadInputs() {
  Payload const * const & f = _input;
  
  if(f && f->watch) {
    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
	       << "Beginning channel traversal for payload " << f->id
	       << " with delay " << _delay
	       << "." << endl;
  }
  Channel<Payload>::ReadInputs();
}

void PayloadChannel::WriteOutputs() {
  Channel<Payload>::WriteOutputs();
  if(_output && _output->watch) {
    *gWatchOut << GetSimTime() << " | " << FullName() << " | "
	       << "Completed channel traversal for payload " << _output->id
	       << "." << endl;
  }
}
