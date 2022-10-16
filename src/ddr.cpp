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
	_data_to_send.resize(2*_core_num);
	_num_flits = config.GetInt("packet_size");
	_flit_width = config.GetInt("flit_width");
	_interleave = config.GetInt("interleave") == 1 ? true : false;
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
	if (!_packet_to_send.empty() && empty)
	{
		_send_data();
	}
	else if (_packet_to_send.empty() && empty && !_data_to_send.empty()) {
		for (int i = 0; i < _data_to_send.size(); i++) {

			bool temp = ceil(double(_data_to_send.front().second.first) / _flit_width) > _num_flits;
			int temp1 = temp ? _num_flits * _flit_width : _data_to_send.front().second.first;
			_packet_to_send.back().second.first = temp ? _num_flits * _flit_width : _data_to_send.front().second.first;//data part, need to add head flit
			_data_to_send.front().second.first = _data_to_send.front().second.first - _packet_to_send.back().second.first;
			assert(_data_to_send.front().second.first >= 0);
			if (_data_to_send.front().second.first == 0) {
				_packet_to_send.push_back(make_pair(true,_data_to_send.front()));
				_packet_to_send.back().second.second.first = temp1;
				_data_to_send.pop_front();
			}
			else {
				_packet_to_send.push_back(make_pair(false, _data_to_send.front()));
				_packet_to_send.back().second.second.first = temp1;
				_data_to_send.push_back(_data_to_send.front());
				_data_to_send.pop_front();
			}
		}
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
				_data_to_send.push_back( make_pair(x, _ofm_message[x].second));//to do when an entry is empty, drain pending_data firstly
			}
		}
	}


}

void DDR::_send_data() {
	int flits = (_packet_to_send.front().second.first - 1) / _flit_width + 1;//data part, need to add head flit
	for (int i = 0; i < flits + 1; i++) {
		Flit* f = Flit::New();
		f->nn_type = 6;
		f->head = i == 0 ? true : false;
		f->tail = i == (flits) ? true : false;
		f->size = _packet_to_send.front().second.second.first;
		f->transfer_id = _packet_to_send.front().second.first;
		f->end = _packet_to_send.front().first;
		f->from_ddr = true;
		if (f->head) {
			if (_packet_to_send.front().second.second.second.size() > 1) {
				f->mflag = true;
				f->mdest.first = _packet_to_send.front().second.second.second;
			}
			else {
				f->dest = _packet_to_send.front().second.second.second[0];
			}
		}
	}
	_packet_to_send.pop_front();
}





	
//}

