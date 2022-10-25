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
	_core_num = config.GetInt("Core_num");
	_num_flits = config.GetInt("packet_size")-1;
	_flit_width = config.GetInt("flit_width");
	_interleave = config.GetInt("interleave") == 1 ? true : false;
	assert(_ddr_id >= 0 && _ddr_id <= _ddr_num);
	vector<int> temp = config.GetIntArray("watch_cores");
	for (auto x : temp) {
		_watch_cores.insert(x);
	}
	vector<int> temp1 = config.GetIntArray("watch_transfer_id");
	for (auto x : temp1) {
		_watch_ids.insert(x);
	}
	for (auto& x : j[to_string(-1)]["in"]) {
			for (auto& y : x["related_ofmap"]) {
				_ifm_to_ofm[x["transfer_id"]].insert(y.get<int>());
			}
	}
	for (auto& x : j[to_string(-1)]["out"]) {
		_ofm_message[x["transfer_id"]].first.first.resize(3);
		_ofm_message[x["transfer_id"]].first.first[0] = x["related_ifmap"].size();
		if (_ddr_id != _ddr_num) {
			_ofm_message[x["transfer_id"]].first.first[1] = x["size"].get<int>()/_ddr_num;
		}
		else {
			_ofm_message[x["transfer_id"]].first.first[1] = x["size"].get<int>() / _ddr_num + x["size"].get<int>() % _ddr_num;
		}
		_ofm_message[x["transfer_id"]].first.first[2] = x["destination"].size();
		_ofm_message[x["transfer_id"]].first.second = x["layer_name"];

		_ofm_message[x["transfer_id"]].second.reserve(x["destination"].size());
		unordered_set<int> temp_dest;
		for (auto& y : x["destination"]) {
			temp_dest.insert(y["id"].get<int>());
		}
		for (auto& y : temp_dest) {
			_ofm_message[x["transfer_id"]].second.push_back(y);
		}
	}
}  


void DDR::run(int time, bool empty, list<Flit*>& _flits_sending) {
//	receive_message(f);
	_time = time;
	if (_time == 6895) {
		int deb = 1;
	}
	//_flits_sending = nullptr;
	if (!_packet_to_send.empty() && empty)
	{
		_send_data(_flits_sending);
	}
	else if (_packet_to_send.empty() && empty && !_data_to_send.empty()) {
		int times = _data_to_send.size();
		for (int i = 0; i < times; i++) {

			bool temp = ceil(double(_data_to_send.front().first.first[1]) / _flit_width) > _num_flits;
			int temp1 = temp ? _num_flits * _flit_width : _data_to_send.front().first.first[1];
//			_packet_to_send.back().first.first[2] = temp ? _num_flits * _flit_width : _data_to_send.front().first.first[1];//data part, need to add head flit
			_data_to_send.front().first.first[1] = _data_to_send.front().first.first[1] - temp1;
			assert(_data_to_send.front().first.first[1] >= 0);
			if (_data_to_send.front().first.first[1] == 0) {
				_packet_to_send.push_back(make_pair(make_pair(true, _data_to_send.front().first),_data_to_send.front().second));
				_packet_to_send.back().first.second.first[1] = temp1;
				_data_to_send.pop_front();
			}
			else {
				_packet_to_send.push_back(make_pair(make_pair(false, _data_to_send.front().first), _data_to_send.front().second));
				_packet_to_send.back().first.second.first[1] = temp1;
				_data_to_send.push_back(_data_to_send.front());
				_data_to_send.pop_front();
			}
		}
		_send_data(_flits_sending);
	}
}



void DDR::receive_message(Flit*f) {
	assert(f->tail);//For request, head is tail ; For data, after tail comes, update buffer.
	if (f->nn_type == 5) {
		
		if (_watch_cores.count(f->src)>0 || _watch_ids.count(f->transfer_id)>0) {
			cout << "this DDR is = " << _ddr_id << " receive requirement transfer_id " << f->transfer_id << " src= " << f->src << " inject time = " << f->ctime << "\n";
		}

		_ofm_message[f->transfer_id].first.first[2] = _ofm_message[f->transfer_id].first.first[2] - 1;
		assert(_ofm_message[f->transfer_id].first.first[2] >= 0);
		if (_ofm_message[f->transfer_id].first.first[2] == 0 && _ofm_message[f->transfer_id].first.first[0] == 0) {
			vector<int> temp(2);
			temp[0] = f->transfer_id;
			temp[1] = _ofm_message[f->transfer_id].first.first[1];
			_data_to_send.push_back(make_pair(make_pair(temp, _ofm_message[f->transfer_id].first.second), _ofm_message[f->transfer_id].second));//to do when an entry is empty, drain pending_data firstly
		}
	}
	if (f->nn_type == 6 ) {
//		if (_watch_ids.count(f->transfer_id) > 0) {
//			cout << " this DDR is = " << _ddr_id << " receive transfer_id " << f->transfer_id << " receive flit " << f->id << " size is = " << f->size << "\n";
//		}
	}
	if (f->nn_type == 6 && f->end) {
		if (_watch_cores.count(f->src) > 0 || _watch_ids.count(f->transfer_id) > 0) {
			cout << "this DDR is = " << _ddr_id << " receive end transfer_id " << f->transfer_id << " src= " << f->src << " inject time = " << f->ctime << "\n";
		}
//		unordered_set<int> temp=_ifm_to_ofm[f->transfer_id];
		if (!_ifm_to_ofm[f->transfer_id].empty()) {
			for (auto& x : _ifm_to_ofm[f->transfer_id]) {
				_ofm_message[x].first.first[0] = _ofm_message[x].first.first[0] - 1;
//				assert(_ofm_message[x].first.first[0]);
				if (_ofm_message[x].first.first[0] == 0 && _ofm_message[x].first.first[2] == 0) {
					
						vector<int> temp(2);
						temp[0] = x;
						temp[1] = _ofm_message[x].first.first[1];
						_data_to_send.push_back(make_pair(make_pair(temp, _ofm_message[x].first.second), _ofm_message[x].second));//to do when an entry is empty, drain pending_data firstly
					
				}
			}
		}
	}

}

void DDR::_send_data(list<Flit*>& _flits_sending) {
	int flits = (_packet_to_send.front().first.second.first[1] - 1) / _flit_width + 1;//data part, need to add head fli
	for (int i = 0; i < flits + 1; i++) {
		Flit* f = Flit::New();
		f->nn_type = 6;
		f->head = i == 0 ? true : false;
		f->ctime = _time;
		f->tail = i == (flits) ? true : false;
		f->size = _packet_to_send.front().first.second.first[1];
		f->layer_name = _packet_to_send.front().first.second.second;
		f->transfer_id = _packet_to_send.front().first.second.first[0];
		
		f->end = _packet_to_send.front().first.first;
		f->from_ddr = true;
		if (f->head) {
			if (_packet_to_send.front().second.size() > 1) {
				f->mflag = true;
				f->mdest.first = _packet_to_send.front().second;
			}
			else {
				f->dest = _packet_to_send.front().second[0];
			}
		}
		_flits_sending.push_back(f);
	}
	if (_watch_ids.count(_packet_to_send.front().first.second.first[0]) > 0 && _packet_to_send.front().first.first ) {
		cout << "this ddr is = " << _ddr_id << " send_end_transfer = "<< _packet_to_send.front().first.second.first[0]<< " at time " << _time << "\n";
	}
	/*
	if (_packet_to_send.front().first.first) {
		cout << " send_data at time " << _time << "ddr \n";
		cout << "this ddr is = " << _ddr_id << " receive transfer_id " << _packet_to_send.front().first.second.first[0] << " src= " << "size = "
			<< _packet_to_send.front().first.second.first[1] << "end = " << _packet_to_send.front().first.first << "\n";
	}*/
	_packet_to_send.pop_front();
}





	
//}

