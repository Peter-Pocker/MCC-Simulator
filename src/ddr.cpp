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
	_ddr_num = config.GetInt("DDR_num");
	_ddr_id = id;
	assert(_ddr_id > 0 && _ddr_id <= _ddr_num);
	for (auto& x : j[-1]["ifmaps"]) {
			for (auto& y : x["destination"]) {
				_ifm_to_ofm[x].insert(y.get<int>());
			}
	}
	for (auto& x : j[-1]["ofmaps"]) {
		_ofm_message[x].first = j[-1]["ofmaps"][x.get<int>()]["source"].size();
		if (_ddr_id != _ddr_num) {
			_ofm_message[x].second.first = j[-1]["ofmaps"][x.get<int>()]["size"].get<int>()/_ddr_num;
		}
		else {
			_ofm_message[x].second.first = j[-1]["ofmaps"][x.get<int>()]["size"].get<int>() / _ddr_num + j[-1]["ofmaps"][x.get<int>()]["size"].get<int>() % _ddr_num;
		}
		int i = 0;
		for (auto& y : j[-1]["ofmaps"][x.get<int>()]["destination"]) {
			_ofm_message[x].second.second[i]=(y["id"].get<int>());
			i + i + 1;
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
		unordered_set<int> temp=_ifm_to_ofm[f->transfer_id];
		for (auto& x : _ifm_to_ofm[f->transfer_id]) {
			_ofm_message[x].first = _ofm_message[x].first - 1;
			assert(_ofm_message[x].first >= 0);
			if (_ofm_message[x].first == 0) {
				_data_to_send.first=
			}
		}
	}
	

}





	
//}

