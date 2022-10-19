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
3.overall finish
*/

/*
* 1.send requirements need add DDR process
* 2.send data also need DDR process
* 3.add DDR placement logic
*/

/*
1.choose to guarantee that all requests are received, the data can be sent; in the future, the sub tile should be fully received.
*/
#include "booksim.hpp"
#include "core.hpp"


Core::Core(const Configuration& config, int id, const nlohmann::json &j)
{  
   _num_obuf = config.GetInt("num_obuf");
   _num_flits= config.GetInt("packet_size")-1;//data flits
   _flit_width = config.GetInt("flit_width");
   _interleave = config.GetInt("interleave")==1 ? true : false;
   _ddr_num = config.GetInt("DDR_num");
   _ddr_id = config.GetIntArray("DDR_routers");
   _sd_gran = config.GetInt("sending_granularity");//How many times did it take to send the data to destination
   _sd_gran_lb= config.GetInt("sending_granularity_lowerbound");
   _core_id = to_string(id);
   _j[_core_id] = j[_core_id];
   _wl_num = _j[_core_id].size();
   _end_message.resize(2);
   _cur_wl_id = -1;
   _cur_id = -1;
   _start_wl_time = -1;
   _start_tile_time = -1;
   _end_tile_time = -1;
   _cp_time = -1;
   _dataready =false;
   pending_data = true;
   cnt1 = 0;
   _wl_fn = true;//the first workload can be viewed as a workload after a virtual previous one.
   _all_fn = false;
   _running = false;
   o_buf.resize(_num_obuf);
   _cur_rc_obuf=0;
   _cur_sd_obuf=-1;
   _mcast_ddr_rid = 0;
   _ddr_rnum = _ddr_id.size() / _ddr_num;
   _cur_tile_id = 0;
   _sd_mini_tile_id = 0;
   _mini_tile_num = -1;
   pending = false;
}  

void Core::_update()
{
	_cur_id = _cur_id + 1;
	assert(_cur_id <= _wl_num);
	if (_cur_id == _wl_num) {
		_wl_end = true;
	}
	else {
		_cur_wl_id = _j[_core_id][_cur_id]["workload_id"].get<int>();
		_cp_time = _j[_core_id][_cur_id]["time"].get<int>();
		_of_size = _j[_core_id][_cur_id]["ofmap_size"].get<int>();
		_cur_tile_id = 0;
		_layer_name = _j[_core_id][_cur_id]["layer_name"].get<string>();
		//try best to use up a packet
		if (_of_size / _sd_gran < (_flit_width * _num_flits)) {
			_sd_gran = (_of_size - 1) / (_flit_width * _num_flits) + 1 > _sd_gran_lb ?
				(_of_size - 1) / (_flit_width * _num_flits) + 1 : _sd_gran_lb;
		}

		_tile_time.assign(_sd_gran - 1, (_cp_time - 1) / _sd_gran + 1);
		_tile_time.push_back(_cp_time - (_sd_gran - 1) * ((_cp_time - 1) / _sd_gran + 1));
		int i = 0;
		pair<vector<int>, unordered_set<int>>temp;
		pair<vector<int>, unordered_set<int>>temp1;
		list<pair<vector<int>, unordered_set<int>>>temp2;
		temp.first.resize(2);
		temp1.first.resize(2);
		for (auto& x : _j[_core_id][_cur_id]["ofmap"]) {
			_send_data_list[x["transfer_id"]] = x["size"].get<int>();;
			temp.first[0] = x["transfer_id"];
			temp1.first[0] = x["transfer_id"];
			temp.first[1] = ceil(double(x["size"].get<int>()) / _sd_gran);
			temp1.first[1] = x["size"].get<int>() - temp.first[1] * (_sd_gran - 1);
			for (auto& y : x["destination"]) {
				if (y["type"].get<string>().compare("DRAM") == 0) {
					temp.second.insert(-1);
					temp1.second.insert(-1);
				}
				else {
					temp.second.insert(y["id"].get<int>());
					temp1.second.insert(y["id"].get<int>());//
					_cur_wl_rq[x["transfer_id"].get<int>()].insert(y["id"].get<int>());
				}
			}
			temp2.assign(_sd_gran - 1, temp);
			temp2.push_back(temp1);
			_tile_size.push_back(temp2);
		}
		for (auto& x : _cur_wl_rq) {
			if (_r_rq_list.count(x.first) != 0) {
				for (auto& p : x.second) {
					if (_r_rq_list[x.first].count(p) != 0) {
						_r_rq_list[x.first].erase(p);
						if (_r_rq_list[x.first].empty()) {
							_r_rq_list.erase(x.first);
						}
						_cur_wl_rq[x.first].erase(p);
						if (_cur_wl_rq[x.first].empty()) {
							_cur_wl_rq.erase(x.first);
						}
					}
				}
			}
		}
			_buffer_update();
			_dataready = _data_ready();
		}
	}

void Core::run(int time, bool empty, list<Flit*>& _flits_sending) {
//	receive_message(f);
	if (_wl_fn &( _dataready && (_cur_rc_obuf!=-1)&&!_wl_end)) {
		_running = true;
		_wl_fn = false;
		_dataready = false;
		_start_wl_time = time;
		_start_tile_time = time;
		_end_tile_time = _start_tile_time + _tile_time.front(); //neglect non-integer part
	}
	if (_running && !_wl_end) {
		if (time == _end_tile_time) {
			_write_obuf();
			_tile_time.pop_front();
//			cout << "_tile_time = " << _tile_time.size() << "\n";
//			cout << "_tile_size = " << _tile_size.size() << "\n";
			_generate_next_rc_obuf_id();
			_generate_next_sd_obuf_id();
			if (_tile_size.empty()) {
				assert(_tile_time.empty());
				_wl_fn = true;
				cnt1 = 0;
			} else if (_cur_rc_obuf == -1) {
				pending = true;
			}
			else {
				_cur_tile_id = _cur_tile_id + 1;
				_start_tile_time = time + 1;
				_end_tile_time = time + _tile_time.front();
			}
		} else if (pending && _cur_rc_obuf != -1) {
			pending = false;
				_cur_tile_id = _cur_tile_id + 1;
				_start_tile_time = time + 1;
				_end_tile_time = time + _tile_time.front();
		}
	}

	if (_wl_fn && !_wl_end && cnt1==0) {
		_running = false;
		_update();
		cnt1 = 1;
		if (!_wl_end) {
			for (auto& p : _rq_to_sent) {
				Flit* f = Flit::New();
				f->nn_type = 5;
				if (_s_rq_list[p][0] < 0 && _interleave) {
					f->mflag = true;
					f->flits_num = 1;
					f->to_ddr = true;
					f->mdest.first.reserve(_ddr_num);
				}
				f->size = _s_rq_list[p][1];
				f->tail = true;
				f->head = true;
				f->transfer_id = p;
				_requirements_to_send.push_back(f);
			}
		}
	}
	//data sending part (connect router)
	bool finish = false;
	//cout << empty << "\n";
	if (!_requirements_to_send.empty() && empty &&!_wl_end) {
		do {
			if (_requirements_to_send.front()->head && _requirements_to_send.front()->to_ddr) {
				for (int i = 0; i < _ddr_num; i++) {
					_requirements_to_send.front()->mdest.first.push_back(_ddr_id[i * _ddr_num + rand() % _ddr_rnum]);
				}
			}
			_flits_sending.push_back(_requirements_to_send.front());
			finish = _requirements_to_send.front()->tail;
			_requirements_to_send.pop_front();
			
		} while (!finish);
	}
	
	else if (_requirements_to_send.empty()  && empty && _cur_sd_obuf!=-1&& _cur_wl_rq.empty()&&!_overall_end) {
		assert(!o_buf[_cur_sd_obuf].empty(),"wrong empty");
		_send_data(_flits_sending);
		if (_wl_end) {
			bool temp = true;
			for (auto& p : o_buf) {
				temp = temp && p.empty();
			}
			if (temp) {
				_overall_end = true;
				_end_time = time;
			}
		}
	}
}


void Core::receive_message(Flit*f) {
	assert(f->tail);//For request, head is tail ; For data, after tail comes, update buffer.
	if (f->nn_type == 5) {
		if (_cur_wl_rq.count(f->transfer_id) == 0) {
			_r_rq_list[f->transfer_id].insert(f->src);
		}
		else {
			_cur_wl_rq[f->transfer_id].erase(f->src);
			if (_cur_wl_rq[f->transfer_id].empty()) {
				_cur_wl_rq.erase(f->transfer_id);
			}
		}
	}
	if (f->nn_type == 6) {
		_s_rq_list[f->transfer_id][1] = _s_rq_list[f->transfer_id][1] - f->size;
		if (f->end) {
			_s_rq_list[f->transfer_id][2] = _s_rq_list[f->transfer_id][2] - 1;
		}
		if (_s_rq_list[f->transfer_id][1] == 0) {
			assert(_s_rq_list[f->transfer_id][2] == 0);
			_core_buffer[f->layer_name].insert(f->transfer_id);
			_left_data.erase(f->transfer_id);
			_dataready = _left_data.empty();
		}
	}
		/*
		cout << " remaining_size = " << _s_rq_list[f->transfer_id][1] << "\n"
			<<"flit source = "<<f->src<<"\n"
			<<"packet_id = "<<f->pid << "\n"
			<<"flit_size = "<<f->size << endl;

	}*/
	/*
	if (f->nn_type == 6 && f->end) {
		_s_rq_list[f->transfer_id][2] = _s_rq_list[f->transfer_id][2] - 1;
		if (_s_rq_list[f->transfer_id][2] == 0) {
//			cout<< " remaining_size = "<<_s_rq_list[f->transfer_id][1]<<"\n";
//			cout << " remaining_end_to_recieve = " << _s_rq_list[f->transfer_id][2] << "\n";
			assert(_s_rq_list[f->transfer_id][1] == 0,"received data no match");
			_core_buffer[f->layer_name].insert(f->transfer_id);
		}
		_left_data.erase(f->transfer_id);
	}*/
	

}
//todo delete out-of-date data
void Core::_buffer_update()
{
	for (auto& x : _j[_core_id][_cur_id]["buffer"]) {
		if (x["newly_added"].get<bool>() == true) {
			for (auto &y : x["source"]) {
				_s_rq_list[y["transfer_id"]].resize(3);
				if (y["type"].get<string>().compare("DRAM") != 0) {
					_s_rq_list[y["transfer_id"]][0]=y["id"].get<int>();
					_s_rq_list[y["transfer_id"]][1]=y["size"].get<int>();
					_s_rq_list[y["transfer_id"]][2] = 1;
				}
				else{
					_s_rq_list[y["transfer_id"]][0]=-1;
					_s_rq_list[y["transfer_id"]][1] = y["size"].get<int>();
					_s_rq_list[y["transfer_id"]][2] = _interleave?_ddr_num:1;//to revise it into ddr group number
				}
				_rq_to_sent.insert(y["transfer_id"].get<int>());
				
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

vector<int>& Core::_check_end() {
	if (_overall_end) {
		_end_message[0] = _overall_end ? 1 : 0;
		_end_message[1] = _end_time;
	}
	return _end_message;
}
void Core::_write_obuf() {
	o_buf[_cur_rc_obuf].reserve(_tile_size.size());
	int temp=0;
	for (auto& x : _tile_size) {
		o_buf[_cur_rc_obuf].push_back(x.front());
		x.pop_front();
		if (x.empty()) {
			_tile_size.erase(_tile_size.begin()+temp);
		}
		temp = temp + 1;
	}
	_mini_tile_num = temp;
}

void Core::_send_data(list<Flit*>& _flits_sending) {
	//if (o_buf[_cur_sd_obuf].empty()) {
	//	if (!_generate_next_sd_obuf_id()) {
	//		return;
	//	}
	//}
	//if(_cur_sd_obuf!=-1) {
//		for (auto& x : o_buf[_cur_sd_obuf]) {
	if (_sd_mini_tile_id < _mini_tile_num - 1) {
		_sd_mini_tile_id = _sd_mini_tile_id + 1;
	}
	else if (_sd_mini_tile_id == _mini_tile_num - 1) {
		_sd_mini_tile_id = 0;
	}
		bool temp = ceil(double(o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1] ) / _flit_width) > _num_flits;
		int flits = temp ? _num_flits : ceil(double(o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1]) / _flit_width);//data part, need to add head flit
		int size = temp ? _flit_width * _num_flits : o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1];
		int transfer_id = o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[0];
		int ddr_initial = o_buf[_cur_sd_obuf][_sd_mini_tile_id].second.size() * _ddr_num;
		bool end = false;
		bool mflas_temp = false;
		o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1] = o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1] - size;
		_send_data_list[transfer_id] = _send_data_list[transfer_id] - size;
						cout << "_send_data"<<_send_data_list[o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[0]] << "\n";
						cout << "obuf" << o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1] <<"\n";
		unordered_set<int> destinations = o_buf[_cur_sd_obuf][_sd_mini_tile_id].second;
		assert(_send_data_list[transfer_id] >= 0);
		assert(o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1] >= 0);

		if (_send_data_list[transfer_id] == 0) {
			end = true;	
			_send_data_list.erase(transfer_id);
		}
		
		if (o_buf[_cur_sd_obuf][_sd_mini_tile_id].first[1] == 0) {
			o_buf[_cur_sd_obuf].erase(o_buf[_cur_sd_obuf].begin() + _sd_mini_tile_id);
			if (_sd_mini_tile_id > 0) {
				_sd_mini_tile_id = _sd_mini_tile_id - 1;
			}
			else
			{
				assert(o_buf[_cur_sd_obuf].empty());
			}
			if (_mini_tile_num > 0) {
				_mini_tile_num = _mini_tile_num - 1;
			}

			if (o_buf[_cur_sd_obuf].empty()) {
				assert(_mini_tile_num == 0);
				if (_cur_rc_obuf == -1) {
					_cur_rc_obuf = _cur_sd_obuf;
				}
				_generate_next_sd_obuf_id();
			}
		}
		for (int i = 0; i < flits+1; i++) {//+1 because there is a head
			Flit* f = Flit::New();
			f->nn_type = 6;
			f->flits_num = flits + 1;
			f->head = i == 0 ? true : false;
			f->tail = i == (flits) ? true : false;
			f->size = size;
			f->mflag = mflas_temp;
			f->transfer_id = transfer_id;
			f->layer_name = _layer_name;
			if (f->head) {				
				if (destinations.size() > 1) {
					f->mflag = true;
					mflas_temp = true;
					for (auto& x : destinations) {
						f->mdest.first.reserve(ddr_initial);
						if (x != -1)
							f->mdest.first.push_back(x);
						else {
							if (!end) {
								if (id_ddr_rel.count(transfer_id) == 0) {
									id_ddr_rel[transfer_id] = rand() % _ddr_num;
									f->mdest.first.push_back(_ddr_id[id_ddr_rel[transfer_id] * _ddr_num + rand() % _ddr_rnum]);
								}
								else {
									if (id_ddr_rel.count(transfer_id) < _ddr_num - 1) {
										id_ddr_rel[transfer_id] = id_ddr_rel[transfer_id] + 1;
										f->mdest.first.push_back(_ddr_id[id_ddr_rel[transfer_id] * _ddr_num + rand() % _ddr_rnum]);
									}
									else if (id_ddr_rel.count(transfer_id) == _ddr_num - 1) {
										id_ddr_rel[transfer_id] = 0;
										f->mdest.first.push_back(_ddr_id[id_ddr_rel[transfer_id] * _ddr_num + rand() % _ddr_rnum]);
									}
								}
							}
							else {
								for (int p = 0; p <= _ddr_num; p++) {
									f->mdest.first.push_back(_ddr_id[p * _ddr_num + rand() % _ddr_rnum]);
								}
							}
						}
					}
				}
				else if (destinations.size() == 1) {
					f->mdest.first.reserve(ddr_initial);
					for (auto& x : destinations) {
						if (x != -1) {
							f->dest = x;
						}
						else {
							if (!end) {
								if (id_ddr_rel.count(transfer_id) == 0) {
									id_ddr_rel[transfer_id] = rand() % _ddr_num;
									f->dest = _ddr_id[id_ddr_rel[transfer_id] * _ddr_num + rand() % _ddr_rnum];
								}
								else {
									if (id_ddr_rel.count(transfer_id) < _ddr_num - 1) {
										id_ddr_rel[transfer_id] = id_ddr_rel.count(transfer_id) + 1;
										f->dest = _ddr_id[id_ddr_rel[transfer_id] * _ddr_num + rand() % _ddr_rnum];
									}
									else if (id_ddr_rel.count(transfer_id) == _ddr_num - 1) {
										id_ddr_rel[transfer_id] = 0;
										f->dest = _ddr_id[id_ddr_rel[transfer_id] * _ddr_num + rand() % _ddr_rnum];
									}
								}
							}
							else {
								f->mflag = true;
								mflas_temp = true;
								for (int p = 0; p < _ddr_num; p++) {
									f->mdest.first.push_back(_ddr_id[p * _ddr_num + rand() % _ddr_rnum]);
								}
							}
						}
					}
				}
			}
			
			if (f->tail) {
				f->end = end;
				if (end) {
					int i = 1;
				}
			}
			
				_flits_sending.push_back(f);
			}
		}


	
//}

