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


}  

void Core::_update()
{
	_cur_id = _cur_id + 1;
	_cur_wl_id = _j[_core_id][_cur_id]["id"];
	_buffer_update();
	_dataready = _data_ready();

}
Flit* send_requirement() {

}
void Core::_buffer_update()
{
	for (auto x : _j[_core_id][_cur_id]["buffer"]) {
		if (x["new_added"] == true) {
			for (auto y : x["transfer_id"])
				_rq_to_sent.insert(y);
		}
	}
}
//when a new workload comes, we check which data has not been in buffer and construct _left_data.
bool Core::_data_ready()
{
	_left_data.clear();
	bool temp = true;
	if (_core_buffer.count(_j[_core_id][_cur_id]["layer_name"]) != 0) {
		for (auto x : _j[_core_id][_cur_id]["ifmap"]["transfer_id"]) {
			if (_core_buffer[_j[_core_id][_cur_id]["layer_name"]].count(x) != 0)
				continue;
			else {
				_left_data.insert(x);
				temp = false;
			}
		}
		if (_j[_core_id][_cur_id].count("weight") != 0) {
			for (auto x : _j[_core_id][_cur_id]["weight"]["transfer_id"]) {
				if (_core_buffer[_j[_core_id][_cur_id]["layer_name"]].count(x) != 0)
					continue;
				else {
					_left_data.insert(x);
					temp = false;
				}
			}
		}
	}
	else {
		for (auto x : _j[_core_id][_cur_id]["ifmap"]["transfer_id"]) {
				_left_data.insert(x);
				temp = false;
		}
		if (_j[_core_id][_cur_id].count("weight") != 0) {
			for (auto x : _j[_core_id][_cur_id]["weight"]["transfer_id"]) {
					_left_data.insert(x);
					temp = false;
			}
		}
	}
	return temp;
	
		
}
