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

#ifndef _GLOBALS_HPP_
#define _GLOBALS_HPP_
#include <string>
#include <vector>
#include <iostream>
#include <map>
#include <list>
#include <utility>

#include "flit.hpp"


/*all declared in main.cpp*/

int GetSimTime();

class Stats;
Stats * GetStats(const std::string & name);

extern bool gPrintActivity;

extern int mcastcount;
extern int total_count;
extern int wiredcount;
extern int wirelesscount;
extern int non_mdnd_hops;
extern int latest_mdnd_hop;


extern int gK;
extern int gN;
extern int gC;

extern int gNodes;

extern bool gTrace;

extern std::ostream * gWatchOut;

//New Code Variables
// extern int count[16][5]; 
extern int print_flit;
extern std::map<int, std::pair<int, int> > hub_mapper; //Bransan <router, pair of <closest hub location, hub id> >
extern std::vector<bool> token_ring; //Bransan boolean token ring
extern std::vector<bool> ready_send; //Bransan ready 
extern std::vector<int> ready_receive;
extern std::vector<std::vector <int> > stats;

// extern int _cur_pid;
// extern int _cur_id;


extern bool token_hold; //Bransan token hold for updating token ring until ready unset


extern int _cur_id;
extern int _cur_pid;
extern std::vector<std::vector<std::list<Flit *> > > _partial_packets;
extern std::vector<std::map<int, Flit *> > _total_in_flight_flits;
extern std::vector<std::map<int, Flit *> > _measured_in_flight_flits;

extern std::vector<std::map<int , int> > wait_clock;

extern std::vector<std::pair<int, int> > time_and_cnt; 

extern std::vector<std::map<int , int> > hwait_clock;

extern std::vector<std::pair<int, int> > htime_and_cnt; 

extern std::map<int, int> f_orig_ctime;
extern std::map<int, int> f_orig_itime;

extern std::map<int, int> f_diff;
extern std::map<int, int> f_diff1;

extern std::map<int, int> uf_orig_ctime;
extern std::map<int, int> uf_orig_itime;

extern std::map<int, int> uf_diff;
extern std::map<int, int> uf_diff1;

extern int total_mcast_dests;

extern int total_mcast_sum;

extern int total_mcast_hops;

extern int total_ucast_dests;

extern int total_ucast_sum;

extern int total_ucast_hops;



#endif
