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
   _flit_width = config.GetInt("flit_width");
   _interleave = config.GetInt("interleave")==1 ? true : false;
   _ddr_num = config.GetInt("DDR_num");
   _ddr_id = config.GetIntArray("DDR_routers");
   _sd_gran = config.GetInt("sending_granularity");
   _sd_gran_lb= config.GetInt("sending_granularity_lowerbound");
   _core_id = id;
   _j[to_string(id)] = j[to_string(id)];
   _cur_wl_id = -1;
   _cur_id = -1;
   _start_wl_time = -1;
   _start_tile_time = -1;
   _end_tile_time = -1;
   _cp_time = -1;
   _dataready =false;
   _wl_fn = true;//the first workload can be viewed as a workload after a virtual previous one.
   _all_fn = false;
   _running = false;
   o_buf.resize(_num_obuf);
   _cur_rc_obuf=-1;
   _cur_sd_obuf=-1;
   _mcast_ddr_rid = 0;
   _ucast_ddr_rid.assign(4,0);
   _ddr_rnum = _ddr_id.size() / _ddr_num;
   _cur_tile_id = 0;
   _sd_mini_tile_id = 0;
}  

void Core::_update()
{
	_cur_id = _cur_id + 1;
	_cur_wl_id = _j[_core_id][_cur_id]["id"].get<int>();
	_cp_time = _j[_core_id][_cur_id]["time"].get<int>();
	_of_size = _j[_core_id][_cur_id]["ofmap"]["size"].get<int>();
	_cur_tile_id = 0;
	//try best to use up a packet
	if (_of_size / _sd_gran < (_flit_width * (_num_flits - 1) / 8)) {
		_sd_gran_r = ceil(double(_of_size) / (_flit_width * (_num_flits - 1) / 8))>_sd_gran_lb? 
			ceil(double(_of_size) / (_flit_width * (_num_flits - 1) / 8)) > _sd_gran_lb : _sd_gran_lb;
	}
	else {
		_sd_gran_r = _sd_gran;
	}
	_tile_time.assign(_sd_gran_r - 1, ceil(double(_time) / _sd_gran_r));
	_tile_time.push_back(_of_size-(_sd_gran_r - 1)* ceil(double(_time) / _sd_gran_r));
	int i = 0;
	for (auto& x : _j[_core_id][_cur_id]["ofmap"]["transfer_id"]) {
		vector<int>temp;
		vector<int>temp1;
		list<vector<int>>temp2;
		temp[0] = x;
		temp1[0] = x;
		temp[1] =ceil(double(_j[_core_id][_cur_id]["ofmap"][x.get<int>()]["size"].get<int>())/ _sd_gran_r);
		temp1[1] = _j[_core_id][_cur_id]["ofmap"][x.get<int>()]["size"].get<int>() - temp[2];
		for (auto& x : _j[_core_id][_cur_id]["ofmap"][x.get<int>()]["destination"]) {
			if (x["type"].get<string>().compare("dram")) {
				temp.push_back(-1);
				temp.push_back(-1);
			}
			else {
				temp[2] = _j[_core_id][_cur_id]["ofmap"][x.get<int>()]["destination"]["id"].get<int>();
				temp1[2] = _j[_core_id][_cur_id]["ofmap"][x.get<int>()]["destination"]["id"].get<int>();//
			}
		}
		temp2.assign(_sd_gran_r - 1, temp);
		temp2.push_back(temp1);
		_tile_size.push_back(temp2);
	}
	_buffer_update();
	_dataready = _left_data.empty() &&_data_ready();

}

list<Flit*> Core::run(int time, bool empty) {
//	receive_message(f);
	_flits_sending.clear();
	if (_wl_fn && _dataready && _generate_next_rc_obuf_id()) {
		_running = true;
		_wl_fn = false;
		_dataready = false;
		_start_wl_time = time;
		_start_tile_time = time;
		_end_tile_time = _start_tile_time + _tile_time.front(); //neglect non-integer part
	}
	if (_running) {
		if (_time == _end_tile_time) {
			_tile_time.pop_front();
			_write_obuf();
			_generate_next_rc_obuf_id();
			if (_cur_rc_obuf != -1) {
				_cur_tile_id = _cur_tile_id + 1;
				_start_tile_time = _time + 1;
				_end_tile_time = _time + _tile_time.front();
			}
		}
		if (_cur_rc_obuf != -1)
		{
			_generate_next_rc_obuf_id();
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
	else if (_requirements_to_send.empty()  && empty) {
		_send_data();
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
				if (y["type"].get<string>().compare("DRAM") == 0) {
					_s_rq_list[y.get<int>()].push_back(y["id"].get<int>());	
				}
				else {
					_s_rq_list[y.get<int>()].push_back(-1);
				}
				_rq_to_sent.insert(y.get<int>());
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
/*
bool Core::_test_obuf() {
	bool temp=false;
	for (auto& p : o_buf) {
		temp = temp || p.empty();
	}
	return temp;
}*/

bool Core::_generate_next_rc_obuf_id() {
	for (int i = 0; i < _num_obuf; i++) {
		if (o_buf[i].empty()) {
			_cur_rc_obuf = i;
			return true;
		}
	}
	_cur_rc_obuf = -1;
	return false;
}

bool Core::_generate_next_sd_obuf_id() {
	for (int i = 0; i < _num_obuf; i++) {
		if (i!=_cur_rc_obuf &&  !o_buf[i].empty()) {
			_cur_sd_obuf = i;
			return true;
		}
	}
	_cur_sd_obuf = -1;
	return false;
}

void Core::_write_obuf() {
	for (auto& x : _tile_size) {
		o_buf[_cur_rc_obuf].push_back(x.front());
		x.pop_front();
	}
}

void Core::_send_data() {
	if (o_buf[_cur_sd_obuf].empty()) {
		if (!_generate_next_sd_obuf_id()) {
			return;
		}
	}
	else {
//		for (auto& x : o_buf[_cur_sd_obuf]) {
		
			bool temp = ceil(double(o_buf[_cur_sd_obuf][_sd_mini_tile_id][2] * 8) / _flit_width) > _num_flits;
			int flits = temp ? _num_flits : ceil(double(o_buf[_cur_sd_obuf][_sd_mini_tile_id][2] * 8) / _flit_width);//data part, need to add head flit
			for (int i = 0; i < flits+1; i++) {
				Flit* f = Flit::New();
				f->nn_type = 6;
				f->head = i == 0 ? true : false;
				f->tail = i == (flits) ? true : false;
				if (f->tail) {
					f->size = temp ? _flit_width * _num_flits / 8 : o_buf[_cur_sd_obuf][_sd_mini_tile_id][2];
					o_buf[_cur_sd_obuf][_sd_mini_tile_id][2] = o_buf[_cur_sd_obuf][_sd_mini_tile_id][2] - f->size;
					assert(o_buf[_cur_sd_obuf][_sd_mini_tile_id][2] >= 0);
					if (o_buf[_cur_sd_obuf][_sd_mini_tile_id][2] = 0) {
						o_buf[_cur_sd_obuf].erase(std::begin(o_buf[_cur_sd_obuf])+ _sd_mini_tile_id);
					}
					f->transfer_id = o_buf[_cur_sd_obuf][_sd_mini_tile_id][0];
				}
				if (f->head) {
					if (o_buf[_cur_sd_obuf][_sd_mini_tile_id].size() > 4) {
						f->mflag = true;
						for (vector<int>::iterator iter = o_buf[_cur_sd_obuf][_sd_mini_tile_id].begin() + 3; iter != o_buf[_cur_sd_obuf][_sd_mini_tile_id].end(); iter++) {
							if (*iter != -1)
								f->mdest.first.push_back(*iter);
							else {
								if (id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]) == 0) {
									id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] = rand() % _ddr_num;
									f->mdest.first.push_back(_ddr_id[id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] * _ddr_num + rand() % _ddr_rnum]);
								}
								else {
									if (id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]) < _ddr_num - 1) {
										id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] = id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]) + 1;
										f->mdest.first.push_back(_ddr_id[id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] * _ddr_num + rand() % _ddr_rnum]);
									}
									else if (id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]) == _ddr_num - 1) {
										id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] = 0;
										f->mdest.first.push_back(_ddr_id[id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] * _ddr_num + rand() % _ddr_rnum]);
									}
								}
							}
						}
					}
					else if (o_buf[_cur_sd_obuf][_sd_mini_tile_id].size() == 4) {
						if (o_buf[_cur_sd_obuf][_sd_mini_tile_id][2] != -1) {
							f->dest = o_buf[_cur_sd_obuf][_sd_mini_tile_id][2];
						}
						else {
							if(id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0])==0){
								id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] = rand() % _ddr_num;
								f->dest = _ddr_id[id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]]*_ddr_num+rand()%_ddr_rnum];
							}
							else {
								if (id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]) < _ddr_num - 1) {
									id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] = id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]) + 1;
									f->dest = _ddr_id[id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] * _ddr_num + rand() % _ddr_rnum];
								}
								else if (id_ddr_rel.count(o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]) == _ddr_num - 1) {
									id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] = 0;
									f->dest = _ddr_id[id_ddr_rel[o_buf[_cur_sd_obuf][_sd_mini_tile_id][0]] * _ddr_num + rand() % _ddr_rnum];
								}
							}
						}
					}
				}
				

				_flits_sending.push_back(f);
			}
		}
	}
//}

