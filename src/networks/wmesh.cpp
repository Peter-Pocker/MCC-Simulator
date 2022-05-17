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

/*kn.cpp
 *
 *Meshs, cube, torus
 *
 */

#include "../booksim.hpp"
#include <vector>
#include <sstream>
#include <ctime>
#include <cassert>
#include "wmesh.hpp"
#include "../random_utils.hpp"
#include "../misc_utils.hpp"
#include <map>
#include <algorithm>
#include <math.h>
 //#include "iq_router.hpp"


WMesh::WMesh( const Configuration &config, const string & name, bool mesh ) :
Network( config, name )
{
  _mesh = mesh;

  _ComputeSize( config );
  _Alloc( );
  _BuildNet( config );
}


void WMesh::inserter_helper(int si, int sj, int ei, int ej, int x)
{
	if(x == 0)
	{
		int pos = ((si+ei)/2)*gK + ((sj +ej)/2);
		_hub_locs.push_back(pos);
		return;
	}
	if(x % 2 == 1)
	{
		x--;
		inserter_helper(si, sj, (si+ei)/2 , ej, x);
		inserter_helper(((si+ei)/2 + 1), sj, ei , ej, x);
	}
	else
	{
		x--;		
		inserter_helper(si, sj, ei , (sj+ej)/2, x);
		inserter_helper(si, ((sj+ej)/2 + 1), ei , ej, x);
	}
}

void WMesh::Location_inserter(unsigned int nhubs)
{
  float expo = log2(nhubs);
  // cout<< expo;
  if(int(expo) != expo)
  {
    Error("Enter hubs as power of 2!!");
  }
	inserter_helper(0 , 0, gK-1, gK-1, expo);	

}

void WMesh::_ComputeSize( const Configuration &config )
{
  _k = config.GetInt( "k" );
  _n = config.GetInt( "n" );
  gK = _k; gN = _n;
  _size     = powi( _k, _n );
  //Bransan added num of hubs here 2
  _nhubs = config.GetInt( "m" );
  if(_nhubs == 0)
    _nhubs = 1;

  if(_nhubs>0)
  {
    token_ring.resize(_nhubs);
    token_ring[0] = true;
    ready_send.resize(_nhubs);
    ready_receive.resize(_nhubs, config.GetInt("received_queue_size"));
  }
  // getchar();
    
  if(_nhubs>_size)
  {
    Error("Too many routers entered\n");
  }

  Location_inserter(_nhubs);
  sort(_hub_locs.begin(), _hub_locs.end());
  cout<<"Hub locations\n";
  for(unsigned int i=0;i<_hub_locs.size();i++)
  {
    cout<<_hub_locs[i]<<"  ";
  }
  cout<<endl;



  
  //Bransan Increased size of chan and chan_cred to accomodate hub
  /*
    No. of 
  1) o/p channels for each router 
      to every other router: 2*n*size
      to connected hub: nhubs
      
  2) o/p channels for each hub
      to every other hub + connected router: nhubs*nhubs
  */
  // _channels = 2*_n*_size + _nhubs + _nhubs*_nhubs; 
  _channels = 2*_n*_size + _nhubs + _nhubs; 
  _p_channels = (_nhubs - 1)*_nhubs;
  _nodes = _size;
  
}

void WMesh::RegisterRoutingFunctions() {

}

pair<int , int> WMesh::closest_hub_finder(int node) 
{
    int dist=0, min_dist=gK*2;
    int min_hub=-1;
    int cur_hub,cur_hub_id = 0;
    int cr,cc, dr, dc;
    for(unsigned int i = 0;i<_hub_locs.size();i++)
    {
        cur_hub=_hub_locs[i];
        cr=node/gK;
        cc=node%gK;
        dr=cur_hub/gK;
        dc=cur_hub%gK;
        dist=abs(cr-dr)+abs(cc-dc);
        if(dist<min_dist)
        {
          min_dist=dist;
          min_hub=cur_hub;
          cur_hub_id = i;
        }
    }
    return make_pair(min_hub,cur_hub_id);
}

void WMesh::_BuildNet( const Configuration &config )
{
  int left_node;
  int right_node;

  int right_input;
  int left_input;

  int right_output;
  int left_output;

  ostringstream router_name;
  ostringstream hub_name;
  //latency type, noc or conventional network
  bool use_noc_latency;
  use_noc_latency = (config.GetInt("use_noc_latency")==1);

  pair <int,int> closest_hub_loc; //calulates distance to closest hub
  
  for ( int node = 0; node < _size; ++node ) {
    
    
    closest_hub_loc = closest_hub_finder(node);
    //Bransan inserting into map hub_mapper
    hub_mapper.insert({node, closest_hub_loc});


    router_name << "router";
    
    if ( _k > 1 ) {
      for ( int dim_offset = _size / _k; dim_offset >= 1; dim_offset /= _k ) {
	      router_name << "_" << ( node / dim_offset ) % _k;
      }
    }
    
    if(!binary_search(_hub_locs.begin(),_hub_locs.end(), node))   //Bransan checking if the node is connected to a hub or not
    {
        _routers[node] = Router::NewRouter( config, this, router_name.str( ), 
        node, 2*_n + 1, 2*_n + 1, 0);
    } 
      
    //Bransan adding extra output port if connected to the hub

    else
    {
      _routers[node] = Router::NewRouter( config, this, router_name.str( ), 
        node, 2*_n + 1 +1, 2*_n + 1 + 1, 0);
    }

    
    _timed_modules.push_back(_routers[node]);
    

    router_name.str("");

    for ( int dim = 0; dim < _n; ++dim ) {

      //find the neighbor 
      left_node  = _LeftNode( node, dim );
      right_node = _RightNode( node, dim );

      //
      // Current (N)ode
      // (L)eft node
      // (R)ight node
      //
      //   L--->N<---R
      //   L<---N--->R
      //

      // torus channel is longer due to wrap around
      int latency = _mesh ? 1 : 2 ;

      //get the input channel number
      right_input = _LeftChannel( right_node, dim );
      left_input  = _RightChannel( left_node, dim );

      //add the input channel
      _routers[node]->AddInputChannel( _chan[right_input], _chan_cred[right_input] );
      _routers[node]->AddInputChannel( _chan[left_input], _chan_cred[left_input] );

      //set input channel latency
      if(use_noc_latency){
        _chan[right_input]->SetLatency( latency );
        _chan[left_input]->SetLatency( latency );
        _chan_cred[right_input]->SetLatency( latency );
        _chan_cred[left_input]->SetLatency( latency );
      } 
      else {
        _chan[left_input]->SetLatency( 1 );
        _chan_cred[right_input]->SetLatency( 1 );
        _chan_cred[left_input]->SetLatency( 1 );
        _chan[right_input]->SetLatency( 1 );
      }
      //get the output channel number
      right_output = _RightChannel( node, dim );
      left_output  = _LeftChannel( node, dim );
      
      //add the output channel
      _routers[node]->AddOutputChannel( _chan[right_output], _chan_cred[right_output] );
      _routers[node]->AddOutputChannel( _chan[left_output], _chan_cred[left_output] );

      //set output channel latency
      if(use_noc_latency){
        _chan[right_output]->SetLatency( latency );
        _chan[left_output]->SetLatency( latency );
        _chan_cred[right_output]->SetLatency( latency );
        _chan_cred[left_output]->SetLatency( latency );
      }
      else {
        _chan[right_output]->SetLatency( 1 );
        _chan[left_output]->SetLatency( 1 );
        _chan_cred[right_output]->SetLatency( 1 );
        _chan_cred[left_output]->SetLatency( 1 );
      }
    }
    //injection and ejection channel, always 1 latency
    _routers[node]->AddInputChannel( _inject[node], _inject_cred[node] );
    _routers[node]->AddOutputChannel( _eject[node], _eject_cred[node] );
    _inject[node]->SetLatency( 1 );
    _eject[node]->SetLatency( 1 );
  }

  for(int i =0;i<_nhubs;i++) {
    hub_name<<"Hub_"<<i;
    _hubs[i]= new Hub(config, this, hub_name.str(), i, _nhubs , _nhubs , 1);
    hub_name.str("");
      //necessary to Read inputs and send outputs and go to internal step in TrafficManager
    _timed_modules.push_back(_hubs[i]);
  }

  unsigned int itr;
  int cur_chan_val=2*gN*_size;

  //hub to router and vice-versa
  for(itr =0;itr<_hub_locs.size();itr++)
  {
      _routers[_hub_locs[itr]]->AddOutputChannel(_chan[cur_chan_val], _chan_cred[cur_chan_val]);
      _hubs[itr]->AddInputChannel(_chan[cur_chan_val], _chan_cred[cur_chan_val]);
      cur_chan_val++;
  }
  for(itr =0;itr<_hub_locs.size();itr++)
  {
      _routers[_hub_locs[itr]]->AddInputChannel(_chan[cur_chan_val], _chan_cred[cur_chan_val]);
      _hubs[itr]->AddOutputChannel(_chan[cur_chan_val], _chan_cred[cur_chan_val]);
      cur_chan_val++;
  }


  cur_chan_val = 0;
  //Hub to hub
  for(unsigned int i =0;i<_hub_locs.size();i++)
  {
    int k = 1;
    //cout<<"i "<<i<<endl;
    for(unsigned int j = (i + 1) % _nhubs;j != i;j =(j + 1) % _nhubs)
    {
        _hubs[i]->AddOutputChannel(_payload_chan[cur_chan_val], _chan_cred[cur_chan_val]);
        _hubs[i]->hub_to_port.insert(pair<int,int>(_hub_locs[j],k));
        _hubs[j]->AddInputChannel(_payload_chan[cur_chan_val], _chan_cred[cur_chan_val]);
        cur_chan_val++;
        k++;
    }
  }
 
  //Bransan setting latency 
  for(int i = 2*gN*_size;i < _channels;i++)
  {
    _chan[i]->SetLatency(1);
    _chan_cred[i]->SetLatency(1);
  }
  
  for(int i = 0;i < cur_chan_val;i++)
  {
    _payload_chan[i]->SetLatency(1);
  }

} 

int WMesh::_LeftChannel( int node, int dim )
{
  // The base channel for a node is 2*_n*node
  int base = 2*_n*node;
  // The offset for a left channel is 2*dim + 1
  int off  = 2*dim + 1;

  return ( base + off );
}

int WMesh::_RightChannel( int node, int dim )
{
  // The base channel for a node is 2*_n*node
  int base = 2*_n*node;
  // The offset for a right channel is 2*dim 
  int off  = 2*dim;
  return ( base + off );
}

int WMesh::_LeftNode( int node, int dim )
{
  int k_to_dim = powi( _k, dim );
  int loc_in_dim = ( node / k_to_dim ) % _k;
  int left_node;
  // if at the left edge of the dimension, wraparound
  if ( loc_in_dim == 0 ) {
    left_node = node + (_k-1)*k_to_dim;
  } else {
    left_node = node - k_to_dim;
  }

  return left_node;
}

int WMesh::_RightNode( int node, int dim )
{
  int k_to_dim = powi( _k, dim );
  int loc_in_dim = ( node / k_to_dim ) % _k;
  int right_node;
  // if at the right edge of the dimension, wraparound
  if ( loc_in_dim == ( _k-1 ) ) {
    right_node = node - (_k-1)*k_to_dim;
  } else {
    right_node = node + k_to_dim;
  }

  return right_node;
}

int WMesh::GetN( ) const
{
  return _n;
}

int WMesh::GetK( ) const
{
  return _k;
}

/*legacy, not sure how this fits into the new scheme of things*/
void WMesh::InsertRandomFaults( const Configuration &config )
{
  int num_fails = config.GetInt( "link_failures" );
  
  if ( _size && num_fails ) {
    vector<long> save_x;
    vector<double> save_u;
    SaveRandomState( save_x, save_u );
    int fail_seed;
    if ( config.GetStr( "fail_seed" ) == "time" ) {
      fail_seed = int( time( NULL ) );
      cout << "SEED: fail_seed=" << fail_seed << endl;
    } else {
      fail_seed = config.GetInt( "fail_seed" );
    }
    RandomSeed( fail_seed );

    vector<bool> fail_nodes(_size);

    for ( int i = 0; i < _size; ++i ) {
      int node = i;

      // edge test
      bool edge = false;
      for ( int n = 0; n < _n; ++n ) {
	if ( ( ( node % _k ) == 0 ) ||
	     ( ( node % _k ) == _k - 1 ) ) {
	  edge = true;
	}
	node /= _k;
      }

      if ( edge ) {
	fail_nodes[i] = true;
      } else {
	fail_nodes[i] = false;
      }
    }

    for ( int i = 0; i < num_fails; ++i ) {
      int j = RandomInt( _size - 1 );
      bool available = false;
      int node = -1;
      int chan = -1;
      int t;

      for ( t = 0; ( t < _size ) && (!available); ++t ) {
	int node = ( j + t ) % _size;
       
	if ( !fail_nodes[node] ) {
	  // check neighbors
	  int c = RandomInt( 2*_n - 1 );

	  for ( int n = 0; ( n < 2*_n ) && (!available); ++n ) {
	    chan = ( n + c ) % 2*_n;

	    if ( chan % 1 ) {
	      available = fail_nodes[_LeftNode( node, chan/2 )];
	    } else {
	      available = fail_nodes[_RightNode( node, chan/2 )];
	    }
	  }
	}
	
	if ( !available ) {
	  cout << "skipping " << node << endl;
	}
      }

      if ( t == _size ) {
	Error( "Could not find another possible fault channel" );
      }

      assert(node != -1);
      assert(chan != -1);
      OutChannelFault( node, chan );
      fail_nodes[node] = true;

      for ( int n = 0; ( n < _n ) && available ; ++n ) {
	fail_nodes[_LeftNode( node, n )]  = true;
	fail_nodes[_RightNode( node, n )] = true;
      }

      cout << "failure at node " << node << ", channel " 
	   << chan << endl;
    }

    RestoreRandomState( save_x, save_u );
  }
}

double WMesh::Capacity( ) const
{
  return (double)_k / ( _mesh ? 8.0 : 4.0 );
}
