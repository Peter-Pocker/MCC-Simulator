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

#ifndef _CORE_HPP_
#define _CORE_HPP_

#include <iostream>
#include <stack>
#include <vector>
#include <utility>
#include <list>
#include <map>
#include <set>
#include <unordered_set>
#include <cassert>


#include "booksim.hpp"
#include "config_utils.hpp"
#include "flit.hpp"
#include "json.hpp"
using namespace std;

class Core {

public:

vector<Flit*> send_data();
Flit* send_requirement();
void receive_message(Flit*f);


private:

  Core(const Configuration& config, int id , const nlohmann::json& j);
  ~Core() {};
  void _update();
  void _buffer_update();
  bool _data_ready();
  nlohmann::json _j;
  int _core_id;
  int _cur_wl_id; // id of current wl
  int _cur_id; //order of current wl
  bool _dataready;  //whether all data of this workload is ready
 // std::unordered_map<int,int> wl_map;
  //for loading data (double ckeck)
  unordered_set<int> _rq_to_sent;//transfer_id
  unordered_map<int, vector<int>> _s_rq_list;//sent_request;the length of the vector is 3, 1st is core_id, 2nd is size, 3rd is 1 or 0(whether finish)
 // unordered_map<int, int>_r_data_list;//receive_data_size;Each entry is decremented and should end up at 0
  //for sending data
  unordered_map<int, unordered_set<int>> _r_rq_list;//received_request,first int is transfer_id£¬set is core list.(unicast has 1 entry, multicast has multiple entry)
  unordered_map<int, int> _s_data_list;//sending_data; no need to distinguish unicast and multicast
  //for buffer record
  unordered_map<string, unordered_set<int>> _core_buffer;//layername,corresponding transfer
  unordered_set<int>_left_data;
};

//ostream& operator<<( ostream& os, const Flit& f );

#endif
