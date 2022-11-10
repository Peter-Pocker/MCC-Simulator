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
void run(int time,bool empty,list<Flit*>&_flits_sending);
void _send_data(list<Flit*>& _flits_sending);
vector<int> &_check_end();
//Flit* send_requirement();
void receive_message(Flit*f);
nlohmann::json& get_json();
Core(const Configuration& config, int id,vector<int>ddr_id, const nlohmann::json& j);
~Core() {};
private:

  void _update();
  void _buffer_update();
  bool _data_ready();
//  bool _test_obuf();//to test whether there is an empty obuf;
  bool _generate_next_rc_obuf_id();
  bool _generate_next_sd_obuf_id();
  void _write_obuf();
  nlohmann::json _j;
  nlohmann::json _j_example;
  string _core_id;
  int _cur_wl_id; // id of current wl
  int _wl_num;//total workloads
  int _cur_id; //order of current wl
  bool _dataready;  //whether all data of this workload is ready
  bool pending_data;
  int _cp_time;//compute time of current workload
  bool _running;//the state of the core, true is on compute, false is stalling (1. data is not in place 2. no output buffer)
  int _num_obuf;//number of output buffer, get from config
  int _num_flits;//number of flits per packet at most;
  int _flit_width;//line width, default 1g hz frequency.
  int _start_wl_time;//starting time of the current workload
  int _start_tile_time; //starting time of the current tile
  int _end_tile_time; //ending time of the current tile
  int _cur_tile_id;
  int _time;
  string _layer_name;
  bool _wl_end;//all workloads are end
  bool _overall_end;//all data sending end
  int _end_time;
  int cnt1;//record update times

  int _sd_gran;//sending granularity of obuf
  int _sd_gran_lb;//sending granularity lower bound


  int _cur_rc_obuf;// the obuf which recieves data;
  int _cur_sd_obuf;// the obuf which sends data;

  int _of_size;

  int _sd_mini_tile_id;

  bool _interleave;
  int _ddr_num;
  int _ddr_rnum;//the number of routers each DDR has
  bool pending;
  //
  int _mcast_ddr_rid;//current requirement should go to ith router (0-_ddr_id.size()/ddr_num-1)
  list<int>_tile_time;
  vector<list<pair<vector<int>, unordered_set<int>>>>_tile_size;
//  int _mini_tile_num;

  //vector<int> _ucast_ddr_rid;//current data unicast should go to ith router of xth DDR. (i is in 0-_ddr_id.size()/ddr_num-1; x is in 0-ddr_num-1)

  vector<int>_ddr_id;
  list<Flit*> _requirements_to_send;//internal partial packets
  list<Flit*> _data_to_send;//internal partial packets
  //list<Flit*> _flits_sending;//output for partial packets
 // std::unordered_map<int,int> wl_map;
  //for loading data (double ckeck)
  unordered_set<int> _rq_to_sent;//transfer_id
  unordered_map<int, pair<unordered_set<string>,vector<int>>> _s_rq_list;//sent_request;the length of the vector is 3, 1st is core_id, 2nd is size, 3rd is number of received end (ddr is >=1)
 // unordered_map<int, int>_r_data_list;//receive_data_size;Each entry is decremented and should end up at 0
  //for sending data
  //unordered_map<int, unordered_set<int>> _r_rq_list;//received_request,first int is transfer_id£¬set is core list.(unicast has 1 entry, multicast has multiple entry)
  unordered_map<int,unordered_set<int>> _r_rq_list;//transfer_id + number
  unordered_map<int, unordered_set<int>> _cur_wl_rq;//the request id of current workload;
  unordered_map<int, int>_send_data_list;//transfer id, data to sent;
  //unordered_map<int, int> _s_data_list;//sending_data; no need to distinguish unicast and multicast
  //for buffer record
  unordered_map<string, unordered_set<int>> _core_buffer;//layername,corresponding transfer
  unordered_set<int>_left_data;
  vector<pair<vector<pair<vector<int>, unordered_set<int>>>,int>> o_buf;//each entry of vector is an output_buffer;
  //                                                        mini tile num
  vector<pair<int,string>> obuf_wl_id;
  //<transfer_id,vector<destination,size>>
  unordered_map<int, int> id_ddr_rel;//ddr relates to transfer_id
  unordered_set<int> _watch_cores;
  unordered_set<int> _watch_ids;
  // signal
  vector<int>_end_message;
  bool _wl_fn;//workload finish
  bool _all_fn;//all workload finsh
  bool _next_start;//the requirement of the current workload is all on site;
};

//ostream& operator<<( ostream& os, const Flit& f );

#endif
