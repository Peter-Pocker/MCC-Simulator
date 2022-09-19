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

/*flit.cpp
 *
 *flit struct is a flit, carries all the control signals that a flit needs
 *Add additional signals as necessary. Flits has no concept of length
 *it is a singluar object.
 *
 *When adding objects make sure to set a default value in this constructor
 */


/*
todo:
1¡¢if a multicast has multiple entries with the same detination. only recod one.

*/
#include "booksim.hpp"
#include "core.hpp"


Core::Core(const Configuration& config, int id, const nlohmann::json &j)
{  
   _core_id = id;
   _j[to_string(id)] = j[to_string(id)];
   _cur_wl_id = _j[to_string(id)][0]["id"];
   _cur_id = 0;
   _buffer_update();
   _dataready = _data_ready();
   _wl_fn = false;
   _all_fn = false;


}  

void Core::_update()
{
	_cur_id = _cur_id + 1;
	_cur_wl_id = _j[_core_id][_cur_id]["id"];
	_buffer_update();
	_dataready = _data_ready();

}

vector<Flit*> Core::run() {
	vector<Flit*> flits_to_send;
//	receive_message(f);

	if (_wl_fn) {
		_buffer_update();
		for (auto& p : _rq_to_sent) {
			Flit* f = Flit::New();
			f->nn_type = 5;
			f->dest = _s_rq_list[p][0];
			f->size = _s_rq_list[p][1];
			f->tail = true;
			f->head = true;
			f->transfer_id = p;
			flits_to_send.push_back(f);
		}
	}
}

void Core::receive_message(Flit*f) {
	assert(f->tail);//For request, head is tail ; For data, after tail comes, update buffer.
	if (f->type == 5) {
		_r_rq_list[f->transfer_id].insert(f->src);
	}
	if (f->type == 6) {
		_s_rq_list[f->transfer_id][2] = _s_rq_list[f->transfer_id][2] - f->size;
	}
	if (f->type == 6 && f->end) {
		_core_buffer[f->layer_name].insert(f->transfer_id);
		assert(_s_rq_list[f->transfer_id][2] = 0);
	}
	

}
void Core::_buffer_update()
{
	for (auto& x : _j[_core_id][_cur_id]["buffer"]) {
		if (x["new_added"].get<bool>() == true) {
			for (auto &y : x["source"]) {
				if (y["type"].get<string>().compare("DRAM") != 0) {
					_rq_to_sent.insert(y.get<int>());
					_s_rq_list[y.get<int>()].push_back(y["id"].get<int>());	
				}
				else {
					_s_rq_list[y.get<int>()].push_back(-1);
				}
				_s_rq_list[y.get<int>()].push_back(y["size"].get<int>());
				_s_rq_list[y.get<int>()].push_back(0);
			}
		}
	}
}
//when a new workload comes, we check which data has not been in buffer and construct _left_data.
bool Core::_data_ready()
{
	_left_data.clear();
	bool temp = true;
	if (_core_buffer.count(_j[_core_id][_cur_id]["layer_name"].get<string>()) != 0) {
		for (auto& x : _j[_core_id][_cur_id]["ifmap"]["transfer_id"]) {
			if (_core_buffer[_j[_core_id][_cur_id]["layer_name"].get<string>()].count(x.get<int>()) != 0)
				continue;
			else {
				_left_data.insert(x.get<int>());
				temp = false;
			}
		}
		if (_j[_core_id][_cur_id].count("weight") != 0) {
			for (auto& x : _j[_core_id][_cur_id]["weight"]["transfer_id"]) {
				if (_core_buffer[_j[_core_id][_cur_id]["layer_name"].get<string>()].count(x.get<int>()) != 0)
					continue;
				else {
					_left_data.insert(x.get<int>());
					temp = false;
				}
			}
		}
	}
	else {
		for (auto& x : _j[_core_id][_cur_id]["ifmap"]["transfer_id"]) {
				_left_data.insert(x.get<int>());
				temp = false;
		}
		if (_j[_core_id][_cur_id].count("weight") != 0) {
			for (auto& x : _j[_core_id][_cur_id]["weight"]["transfer_id"]) {
					_left_data.insert(x.get<int>());
					temp = false;
			}
		}
	}
	return temp;
	
		
}
