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
#include "ddr.hpp"


DDR::DDR(const Configuration& config, int id, const nlohmann::json &j)
{  
	for (auto& x : j[-1]["ofmaps"]) {
			for (auto& y : x["source"]) {
				_ofm_to_ifm[x].insert(y.get<int>());
			}
	}
}  


list<Flit*> DDR::run(int time, bool empty) {
//	receive_message(f);
	_flits_sending.clear();
	if (_wl_fn && _dataready && _cur_rc_obuf!=-1) {
		_running = true;
		_wl_fn = false;
		_dataready = false;
		_start_wl_time = time;
		_start_tile_time = time;
		_end_tile_time = _start_tile_time + _tile_time.front(); //neglect non-integer part
	}
	if (_running) {
		if (_time == _end_tile_time) {
			_generate_next_rc_obuf_id();
			if (_cur_rc_obuf == -1) {
				pending = true;
			}
		}
		if ((_time == _end_tile_time && _cur_rc_obuf!=-1)||(pending&& _cur_rc_obuf != -1)) {
			pending = false;
			_tile_time.pop_front();
			_write_obuf();
			if (_cur_rc_obuf != -1) {
				_cur_tile_id = _cur_tile_id + 1;
				_start_tile_time = _time + 1;
				_end_tile_time = _time + _tile_time.front();
			}
		}

		if (_tile_size.empty()) {
			assert(_tile_time.empty());
			_wl_fn = true;
		}

	}


	if (_wl_fn) {
		_running = false;
		_update();
		for (auto& p : _rq_to_sent) {
			Flit* f = Flit::New();
			f->nn_type = 5;
			if(_s_rq_list[p][0]<0 && _interleave){
				f->to_ddr = true;
				f->mflag = true;
			}
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
			vector<int> temp;
			if (_requirements_to_send.front()->head && _requirements_to_send.front()->to_ddr) {
				int i = 0;
				for (int p = rand()%_ddr_rnum; p < _ddr_num; p = p + _ddr_rnum) {
					temp[i] = _ddr_id[p];
					i = i + 1;
				}
			}
			_flits_sending.push_back(_requirements_to_send.front());
			finish = _requirements_to_send.front()->tail;
			/*
			if (finish && _mcast_ddr_rid<_ddr_rnum-1) {
				_mcast_ddr_rid = _mcast_ddr_rid + 1;
			}
			else if (finish && _mcast_ddr_rid == _ddr_rnum - 1) {
				_mcast_ddr_rid = 0;
			}*/
			_requirements_to_send.pop_front();
			
		} while (finish);
	}
	else if (_requirements_to_send.empty()  && empty && _cur_sd_obuf!=-1) {
		assert(!o_buf[_cur_sd_obuf].empty());
		_send_data();
	}
	return _flits_sending;
}



void DDR::receive_message(Flit*f) {
	assert(f->tail);//For request, head is tail ; For data, after tail comes, update buffer.
	if (f->nn_type == 5) {
		_r_rq_list.insert(f->transfer_id);
	}
	if (f->nn_type == 6 && f->end) {
		
	}
	

}
//todo delete out-of-date data
void DDR::_buffer_update()
{
	for (auto& x : _j[_core_id][_cur_id]["buffer"]) {
		if (x["new_added"].get<bool>() == true) {
			for (auto &y : x["source"]) {
				if (y["type"].get<string>().compare("DRAM") == 0) {
					_s_rq_list[y.get<int>()][0]=y["id"].get<int>();	
					_s_rq_list[y.get<int>()][1]=y["size"].get<int>();
					_s_rq_list[y.get<int>()][2] = 1;
				}
				else if (y["type"].get<string>().compare("DRAM") == 1){
					_s_rq_list[y.get<int>()][0]=-1;
					_s_rq_list[y.get<int>()][1] = _interleave?_ddr_num:1;//to revise it into ddr group number
				}
				_rq_to_sent.insert(y.get<int>());
				
			}
		}
	}
}
//when a new workload comes, we check which data has not been in buffer and construct _left_data.
bool DDR::_data_ready()
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
/*
bool DDR::_test_obuf() {
	bool temp=false;
	for (auto& p : o_buf) {
		temp = temp || p.empty();
	}
	return temp;
}*/

bool DDR::_generate_next_rc_obuf_id() {
	for (int i = 0; i < _num_obuf; i++) {
		if (o_buf[i].empty()) {
			_cur_rc_obuf = i;
			return true;
		}
	}
	_cur_rc_obuf = -1;
	return false;
}

bool DDR::_generate_next_sd_obuf_id() {
	for (int i = 0; i < _num_obuf; i++) {
		if (i!=_cur_rc_obuf &&  !o_buf[i].empty()) {
			_cur_sd_obuf = i;
			return true;
		}
	}
	_cur_sd_obuf = -1;
	return false;
}

void DDR::_write_obuf() {
	for (auto& x : _tile_size) {
		o_buf[_cur_rc_obuf].push_back(x.front());
		x.pop_front();
	}
}




	
//}

