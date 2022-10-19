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


using namespace std;
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
    std::cout<<"warning statistics "<<name<<" not found"<<endl;
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

int total_ucast_dests = 0;
int total_ucast_sum = 0;
int total_ucast_hops = 0;


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
map<int, int> f_orig_itime;
map<int, int> f_diff;
map<int, int> f_diff1;

map<int, int> uf_orig_ctime;
map<int, int> uf_orig_itime;
map<int, int> uf_diff;
map<int, int> uf_diff1;

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

  std::cout<<"Total run time "<<total_time<<endl;

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
  std::cout <<"\nHub Condition Statistics per hub"<<endl;
  std::cout <<"  ";
  for(int i = 0; i < 8; i++)
  {
      std::cout <<setw(8)<<"C"<<i;
  }
  std::cout <<setw(10)<<"Sum "<<endl;
  for(int id = 0; id < nhubs; id++)
  {
      std::cout <<"H"<<id;
    for(int j = 0 ;j < 8; j++){
        std::cout <<setw(9)<<stats[id][j];
      sum+=stats[id][j];
    }
    std::cout <<setw(9)<<sum<<endl;
    sum = 0;
  }
  std::cout <<endl;

  
  std::cout <<" Packet wait time at Router\n";
  std::cout <<setw(10)<<"Time"<<setw(10)<<"Count"<<setw(10)<<"Avg"<<endl;

  for(int i = 0; i < nhubs; i++)
  {
      std::cout <<setw(10)<<time_and_cnt[i].first<<setw(10)<<time_and_cnt[i].second<<setw(10)<<time_and_cnt[i].first/time_and_cnt[i].second;
      std::cout <<endl;
  }

  std::cout <<"\nPacket wait time at Hub including token delay\n";
 std::cout<<setw(10)<<"Time"<<setw(10)<<"Count"<<setw(10)<<"Avg"<<endl;

  for(int i = 0; i < nhubs; i++)
  {
   std::cout<<setw(10)<<htime_and_cnt[i].first<<setw(10)<<htime_and_cnt[i].second<<setw(10)<<htime_and_cnt[i].first/htime_and_cnt[i].second;
   std::cout<<endl;
  }
}


int main(int argc, char** argv)
{


    BookSimConfig config;

    /*
    if ( !ParseArgs( &config, argc, argv ) ) {
      cerr << "Usage: " << argv[0] << " configfile... [param=value...]" << endl;
      return 0;
   }
  */

  /*initialize routing, traffic, injection functions
   */
    InitializeRoutingMap(config);

    gPrintActivity = (config.GetInt("print_activity") > 0);
    gTrace = (config.GetInt("viewer_trace") > 0);

    string watch_out_file = config.GetStr("watch_out");
    if (watch_out_file == "") {
        gWatchOut = NULL;
    }
    else if (watch_out_file == "-") {
        gWatchOut = &cout;
    }
    else {
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

    for (int i = 0; i < nhubs; i++)
        stats[i].resize(8);

    int mcast_percent = config.GetInt("mcast_percent");


    bool result = Simulate(config);
    if (nhubs > 1)
        hubstatistacks(nhubs);

    int packet_size = config.GetInt("packet_size");
    cout << endl << endl;
    std::cout << "Total number of packets " << total_count << endl;
    std::cout << "Number of Multicast packets " << mcastcount  << endl;
    std::cout << "Number of Wired multicast packets " << wiredcount << endl;
    std::cout << "Number of wireless multicast packets " << wirelesscount << endl;

    int count = 0;
    int sum = 0;
    float avg = 0;
    int max = 0;
    int maxid = 0;
    int min = INT_MAX;
    int minid = 0;
    for (map<int, int>::iterator i = f_diff.begin(); i != f_diff.end(); i++)
    {
        if (i->first > -1) {
            sum += i->second;
            if (i->second > max) {
                max = i->second;
                maxid = i->first;
            }
            if (i->second < min) {
                min = i->second;
                minid = i->first;
            }
            count++;
        }
    }
    int count1 = 0;
    int sum1 = 0;
    float avg1 = 0;
    int max1 = 0;
    int maxid1 = 0;
    int min1 = INT_MAX;
    int minid1 = 0;
    for (map<int, int>::iterator i1 = f_diff1.begin(); i1 != f_diff1.end(); i1++)
    {
//        f_diff1;
//        cout << "m fid = " << i1->first << " time cost = " << i1->second << "\n";
        if (i1->first > -1) {
            sum1 += i1->second;

            if (i1->second > max1) {
                max1 = i1->second;
                maxid1 = i1->first;
            }
            if (i1->second < min1) {
                min1 = i1->second;
                minid1 = i1->first;
            }
            count1++;
        }
    }

    int ucount = 0;
    int usum = 0;
    float uavg = 0;
    int umax = 0;
    int umaxid = 0;
    int umin = INT_MAX;
    int uminid = 0;
    for (map<int, int>::iterator i2 = uf_diff.begin(); i2 != uf_diff.end(); i2++)
    {
 //       cout << " fid = " << i2->first << " time cost = " << i2->second << "\n";
        usum += i2->second;
        if (i2->second > umax) {
            umax = i2->second;
            umaxid = i2->first;
        }
        if (i2->second < umin) {
            umin = i2->second;
            uminid = i2->first;
        }
        ucount++;
    }
    int ucount1 = 0;
    int usum1 = 0;
    float uavg1 = 0;
    int umax1 = 0;
    int umaxid1 = 0;
    int umin1 = INT_MAX;
    int uminid1 = 0;
    for (map<int, int>::iterator i3 = uf_diff1.begin(); i3 != uf_diff1.end(); i3++)
    {
        usum1 += i3->second;
        if (i3->second > umax1) {
            umax1 = i3->second;
            umaxid1 = i3->first;
        }
        if (i3->second < umin1) {
            umin1 = i3->second;
            uminid1 = i3->first;
        }
        ucount1++;

    }
    //std::cout << " ucount = " << ucount << " ucount1 =" << ucount1;
    
    if (count != 0)
    {
        avg = ((float)sum) / count;
        std::cout << "Average multicast transaction latency (from creating time): ";
        std::cout << avg << endl;
    }
    if (count != 0)
    {

        std::cout << "Max multicast transaction latency (from creating time): ";
        std::cout << max << " (fid = " << maxid << ")" << endl;
    }
    if (count != 0)
    {

        std::cout << "Min multicast transaction latency (from creating time): ";
        std::cout << min << " (fid = " << minid << ")" << endl;
    }
    if (count1 != 0)
    {
        avg1 = ((float)sum1) / count1;
        std::cout << "Average multicast transaction latency (from inject time): ";
        std::cout << avg1 << endl;
    }
    if (count1 != 0)
    {

        std::cout << "Max multicast transaction latency (from inject time): ";
        std::cout << max1 << " (fid = " << maxid1 << ")" << endl;
    }
    if (count1 != 0)
    {

        std::cout << "Min multicast transaction latency (from inject time): ";
        std::cout << min1 << " (fid = " << minid1 << ")" << endl;
    }
    /*
   std::cout<<"Average packet latency for multicast flits : ";
    if(total_mcast_dests > 0)
     std::cout<<((float)total_mcast_sum)/total_mcast_dests<<endl;
    else
    {
     std::cout<<"No mcast flits"<<endl;
    }
  */
  // std::cout<<"Average hops for multicast flits : ";
    if (total_mcast_dests > 0)
    {
        //   std::cout<<((float)total_mcast_hops)/(mcastcount/packet_size)<<endl;
        std::cout << "Total number of mcast hops is " << total_mcast_hops << endl;
    }

    else
    {
        std::cout << "No mcast flits" << endl;
    }


    /*
  cout<<"Average packet latency for multicast flits : ";
  if(total_mcast_dests > 0)
   std::cout<<((float)total_mcast_sum)/total_mcast_dests<<endl;
  else
  {
   std::cout<<"No mcast flits"<<endl;
  }
  */
  // std::cout << "Average hops for unicast flits : ";
  //  if (total_mcast_dests > 0)
  //  {
  //     std::cout << ((float)total_ucast_hops) / (mcastcount / packet_size) << endl;
    std::cout << "Total number of ucast hops is " << total_ucast_hops << endl;
    //  }

    //  else
     // {
    //     std::cout << "No mcast flits" << endl;
     // for unicast
      //}

    if (ucount != 0)
    {
        uavg = ((float)usum) / ucount;
        std::cout << "Average unicast transaction latency (from creating time): ";
        std::cout << uavg << "(total latency = " << usum << " count = " << ucount << ")" << endl;
    }
    if (ucount != 0)
    {

        std::cout << "Max unicast transaction latency (from creating time): ";
        std::cout << umax << " (fid = " << umaxid << ")" << endl;
    }
    if (ucount != 0)
    {

        std::cout << "Min unicast transaction latency (from creating time): ";
        std::cout << umin << " (fid = " << uminid << ")" << endl;
    }
    if (ucount1 != 0)
    {
        uavg1 = ((float)usum1) / ucount1;
        std::cout << "Average unicast transaction latency (from inject time): ";
        std::cout << uavg1 << "(total latency = " << usum1 << " count = " << ucount1 << ")" << endl;
    }
    if (ucount1 != 0)
    {

        std::cout << "Max unicast transaction latency (from inject time): ";
        std::cout << umax1 << " (fid = " << umaxid1 << ")" << endl;
    }
    if (ucount1 != 0)
    {

        std::cout << "Min unicast transaction latency (from inject time): ";
        std::cout << umin1 << " (fid = " << uminid1 << ")" << endl;
    }
}
  //Please try fixing this in the future. This code is a hack and is wrong.
//This is needed because for some reason when m=0, it generates one more
//multicast packet as compared to when m>0. And for some weird reason this
//only happens when k=16 and not when k=8
//==================================================================//
  /*
  if (nhubs == 0 && gK == 16)
      non_mdnd_hops -= latest_mdnd_hop;
  //==================================================================//
 std::cout << "Number of hops without using MDND " << non_mdnd_hops << endl;

 std::cout << "Delta = " << (1 - (float)(total_mcast_hops) / non_mdnd_hops) << endl;
  return result ? -1 : 0;
  }*/

