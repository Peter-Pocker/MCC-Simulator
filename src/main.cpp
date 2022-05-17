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

/*main.cpp
 *
 *The starting point of the network simulator
 *-Include all network header files
 *-initilize the network
 *-initialize the traffic manager and set it to run
 *
 *
 */
#include <time.h>

#include <string>
#include <cstdlib>
#include <iostream>
#include <fstream>
#include <iomanip>


#include <sstream>
#include "booksim.hpp"
#include "routefunc.hpp"
#include "traffic.hpp"
#include "booksim_config.hpp"
#include "trafficmanager.hpp"
#include "random_utils.hpp"
#include "networks/network.hpp"
#include "injection.hpp"
#include "power/power_module.hpp"



///////////////////////////////////////////////////////////////////////////////
//Global declarations
//////////////////////

 /* the current traffic manager instance */
TrafficManager * trafficManager = NULL;

int GetSimTime() {
  return trafficManager->getTime();
}

class Stats;
Stats * GetStats(const std::string & name) {
  Stats* test =  trafficManager->getStats(name);
  if(test == 0){
    cout<<"warning statistics "<<name<<" not found"<<endl;
  }
  return test;
}

/* printing activity factor*/
bool gPrintActivity;

int gK;//radix
int gN;//dimension
int gC;//concentration

int gNodes;

//generate nocviewer trace
bool gTrace;

ostream * gWatchOut;



/////////////////////////////////////////////////////////////////////////////
int total_mcast_dests = 0;
int total_mcast_sum = 0;
int total_mcast_hops = 0;


int mcastcount = 0;
int total_count = 0;
int wirelesscount = 0;
int wiredcount = 0;
int non_mdnd_hops = 0;
int latest_mdnd_hop = 0;
//Bransan map declare
map<int, pair<int,int> > hub_mapper;
//Bransan vector declaration for the token ring
vector<bool> token_ring;
//Bransan vector declaration of the ready data structure
vector<bool> ready_send;

//Bransan ready queue status for hubs
vector<int> ready_receive;

vector< vector <int> > stats;

bool token_hold = false;


int _cur_id;
int _cur_pid;
vector<vector<list<Flit *> > > _partial_packets;
vector<map<int, Flit *> > _total_in_flight_flits;
vector<map<int, Flit *> > _measured_in_flight_flits;

map<int, int> f_orig_ctime;
map<int, int> f_diff;


vector<map<int , int> > wait_clock;
vector<pair<int, int> > time_and_cnt; 

vector<map<int , int> > hwait_clock;
vector<pair<int, int> > htime_and_cnt; 

bool Simulate( BookSimConfig const & config )
{
  vector<Network *> net;

  int subnets = config.GetInt("subnets");
  // cout<<"subnets "<<subnets<<endl<<endl<<endl;
  /*To include a new network, must register the network here
   *add an else if statement with the name of the network
   */
  net.resize(subnets);
  for (int i = 0; i < subnets; ++i) {
    ostringstream name;
    name << "network_" << i;
    net[i] = Network::New( config, name.str() );
  }

  /*tcc and characterize are legacy
   *not sure how to use them 
   */

  assert(trafficManager == NULL);
  trafficManager = TrafficManager::New( config, net ) ;

  /*Start the simulation run
   */

  double total_time; /* Amount of time we've run */
  // struct timeval start_time, end_time; /* Time before/after user code */
  // total_time = 0.0;
   //gettimeofday(&start_time, NULL);

  time_t start;
  time_t end;
  time(&start);


  bool result = trafficManager->Run();
  time(&end);
  total_time = end - start;

  cout<<"Total run time "<<total_time<<endl;

  for (int i=0; i<subnets; ++i) {

    ///Power analysis
    if(config.GetInt("sim_power") > 0){
      Power_Module pnet(net[i], config);
      pnet.run();
    }

    delete net[i];
  }

  delete trafficManager;
  trafficManager = NULL;

  return result;
}

void hubstatistacks(int nhubs)
{
  int sum = 0;
  cout<<"\nHub Condition Statistics per hub"<<endl;
  cout<<"  ";
  for(int i = 0; i < 8; i++)
  {
    cout<<setw(8)<<"C"<<i;
  }
  cout<<setw(10)<<"Sum "<<endl;
  for(int id = 0; id < nhubs; id++)
  {
    cout<<"H"<<id;
    for(int j = 0 ;j < 8; j++){
      cout<<setw(9)<<stats[id][j];
      sum+=stats[id][j];
    }
    cout<<setw(9)<<sum<<endl;
    sum = 0;
  }
  cout<<endl;

  
  cout<<" Packet wait time at Router\n";
  cout<<setw(10)<<"Time"<<setw(10)<<"Count"<<setw(10)<<"Avg"<<endl;

  for(int i = 0; i < nhubs; i++)
  {
    cout<<setw(10)<<time_and_cnt[i].first<<setw(10)<<time_and_cnt[i].second<<setw(10)<<time_and_cnt[i].first/time_and_cnt[i].second;
    cout<<endl;
  }

  cout<<"\nPacket wait time at Hub including token delay\n";
  cout<<setw(10)<<"Time"<<setw(10)<<"Count"<<setw(10)<<"Avg"<<endl;

  for(int i = 0; i < nhubs; i++)
  {
    cout<<setw(10)<<htime_and_cnt[i].first<<setw(10)<<htime_and_cnt[i].second<<setw(10)<<htime_and_cnt[i].first/htime_and_cnt[i].second;
    cout<<endl;
  }
}


int main( int argc, char **argv )
{


  
  BookSimConfig config;


  if ( !ParseArgs( &config, argc, argv ) ) {
    cerr << "Usage: " << argv[0] << " configfile... [param=value...]" << endl;
    return 0;
 } 

  
  /*initialize routing, traffic, injection functions
   */
  InitializeRoutingMap( config );

  gPrintActivity = (config.GetInt("print_activity") > 0);
  gTrace = (config.GetInt("viewer_trace") > 0);
  
  string watch_out_file = config.GetStr( "watch_out" );
  if(watch_out_file == "") {
    gWatchOut = NULL;
  } else if(watch_out_file == "-") {
    gWatchOut = &cout;
  } else {
    gWatchOut = new ofstream(watch_out_file.c_str());
  }
  

  /*configure and run the simulator
   */

  int nhubs = config.GetInt("m");
  stats.resize(nhubs);
  wait_clock.resize(nhubs);
  time_and_cnt.resize(nhubs);
  hwait_clock.resize(nhubs);
  htime_and_cnt.resize(nhubs);

  for(int i =0 ; i < nhubs ; i++)
    stats[i].resize(8);

  int mcast_percent = config.GetInt("mcast_percent");
  

  bool result = Simulate( config );
  if(nhubs>1)
    hubstatistacks(nhubs);

  int packet_size = config.GetInt("packet_size");
  cout<<endl<<endl;
  cout<<"Total number of packets "<<total_count/packet_size<<endl;
  cout<<"Number of Multicast packets "<<mcastcount/packet_size<<endl;
  cout<<"Number of Wired multicast packets "<<wiredcount<<endl;
  cout<<"Number of wireless multicast packets "<<wirelesscount<<endl;

  int count = 0;
  int sum = 0;
  float avg = 0;
  for(map<int, int>::iterator i = f_diff.begin(); i != f_diff.end(); i++)
  {
    sum += i->second;
    count++;
  }
  if(count != 0)
  {
    avg = ((float)sum)/count;
    cout<<"Average multicast transaction latency : ";
    cout<<avg<<endl;
  }

  cout<<"Average packet latency for multicast flits : ";
  if(total_mcast_dests > 0)
    cout<<((float)total_mcast_sum)/total_mcast_dests<<endl;
  else
  {
    cout<<"No mcast flits"<<endl;
  }

  cout<<"Average hops for multicast flits : ";
  if(total_mcast_dests > 0)
  {
    cout<<((float)total_mcast_hops)/(mcastcount/packet_size)<<endl;
    cout<<"Total number of mcast hops is "<<total_mcast_hops<<endl;
  }

  else
  {
    cout<<"No mcast flits"<<endl;
  }

  //Please try fixing this in the future. This code is a hack and is wrong.
  //This is needed because for some reason when m=0, it generates one more
  //multicast packet as compared to when m>0. And for some weird reason this
  //only happens when k=16 and not when k=8
  //==================================================================//
  if(nhubs == 0 && gK == 16)
    non_mdnd_hops -= latest_mdnd_hop;
  //==================================================================//
  cout<<"Number of hops without using MDND "<<non_mdnd_hops <<endl;

  cout<<"Delta = "<<(1 - (float)(total_mcast_hops)/non_mdnd_hops)<<endl;
  return result ? -1 : 0;
}
