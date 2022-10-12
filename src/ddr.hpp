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

class DDR {

public:
list<Flit*> run(int time,bool empty);


void _send_data();
//Flit* send_requirement();
void receive_message(Flit*f);
DDR(const Configuration& config, int id, const nlohmann::json& j);
~DDR() {};
private:
	unordered_set<int> _r_ts_list;//received transfer
	unordered_set<int> _r_rq_list;//received requirest
	vector<pair<int, pair<vector<int>, int>>> _data_to_send;
	unordered_map<int, unordered_set<int>> _ofm_to_ifm;
};


#endif
