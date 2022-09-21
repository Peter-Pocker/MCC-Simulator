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
1.if a multicast has multiple entries with the same detination. only recod one.
2.all vector should be initialize size
*/

/*
* 1.send requirements need add DDR process
* 2.send data also need DDR process
* 3.add DDR placement logic
*/
#include "booksim.hpp"
#include "core.hpp"


Core::Core(const Configuration& config, int id, const nlohmann::json &j)
{  
   _num_obuf = config.GetInt("num_obuf");
   _num_flits= config.GetInt("packet_size");
   _core_id = id;
   _j[to_string(id)] = j[to_string(id)];
   _cur_wl_id = _j[to_string(id)][0]["id"];
   _cur_id = 0;
   _start_wl_time = -1;
   _start_tile_time = -1;
   _end_tile_time = -1;
   _cp_time = _j[_core_id][_cur_id]["time"];
   _sd_gran = config.GetInt("sending_granularity");
   _buffer_update();
   _dataready = _left_data.empty() && _data_ready();
   _wl_fn = true;//the first workload can be viewed as a workload after a virtual previous one.
   _all_fn = false;
   _running = false;
   o_buf.resize(_num_obuf);
   _cur_rc_obuf=-1;
   _cur_sd_obuf=-1;
}  

void Core::_update()
{
	_cur_id = _cur_id + 1;
	_cur_wl_id = _j[_core_id][_cur_id]["id"];
	_cp_time = _j[_core_id][_cur_id]["time"];
	_buffer_update();
	_dataready = _left_data.empty() &&_data_ready();

}

list<Flit*> Core::run(int time, bool empty) {
//	receive_message(f);
	_flits_sending.clear();
	if (_wl_fn && _dataready && _test_obuf()) {
		_running = true;
		_wl_fn = false;
		_dataready = false;
		_start_wl_time = time;
		_start_tile_time = time;
		_end_tile_time = _start_tile_time + _cp_time / _sd_gran; //neglect non-integer part
		_generate_next_obuf_id();
	}
	if (_running) {
		if (_time == _end_tile_time) {
			_write_obuf();
			_generate_next_obuf_id();
			if (_cur_rc_obuf != -1) {
				_start_tile_time = _time + 1;
				_end_tile_time = _time + _cp_time / _sd_gran;
			}
		}
		if (_cur_rc_obuf == -1)
		{
			_generate_next_obuf_id();
		}

	}


	if (_wl_fn) {
		_running = false;
		_update();
		for (auto& p : _rq_to_sent) {
			Flit* f = Flit::New();
			f->nn_type = 5;
			f->dest = _s_rq_list[p][0];
			f->size = _s_rq_list[p][1];
			f->tail = true;
			f->head = true;
			f->transfer_id = p;
			_requirements_to_send.push_back(f);
		}
	}
	//data sending part (connect router)
	bool finish = false;
	if (!_requirements_to_send.empty() && empty) {
		do {
			assert(_requirements_to_send.front()->head);
			_flits_sending.push_back(_requirements_to_send.front());
			finish = _requirements_to_send.front()->tail;
			_requirements_to_send.pop_front();
			
		} while (finish);
	}
	else if (_requirements_to_send.empty() && !o_buf[_cur_sd_obuf].empty() && empty) {
		do {
			assert(_data_to_send.front()->head);
			_flits_sending.push_back(_data_to_send.front());
			finish = _data_to_send.front()->tail;
			_data_to_send.pop_front();
			
		} while (finish);
	}
	return _flits_sending;
}

void Core::_compute() {
	
}

void Core::receive_message(Flit*f) {
	assert(f->tail);//For request, head is tail ; For data, after tail comes, update buffer.
	if (f->nn_type == 5) {
		_r_rq_list[f->transfer_id].insert(f->src);
	}
	if (f->nn_type == 6) {
		_s_rq_list[f->transfer_id][2] = _s_rq_list[f->transfer_id][2] - f->size;
	}
	if (f->nn_type == 6 && f->end) {
		_core_buffer[f->layer_name].insert(f->transfer_id);
		assert(_s_rq_list[f->transfer_id][2] = 0);
		_left_data.erase(f->transfer_id);
	}
	

}
//todo delete out-of-date data
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

bool Core::_test_obuf() {
	bool temp=false;
	for (auto& p : o_buf) {
		temp = temp || p.empty();
	}
	return temp;
}

void Core::_generate_next_obuf_id() {
	for (int i = 0; i < _num_obuf; i++) {
		if (o_buf[i].empty()) {
			_cur_rc_obuf = i;
			return;
		}
	}
	_cur_rc_obuf = -1;
}

void Core::_write_obuf() {
	
}

