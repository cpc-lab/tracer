#!/usr/bin/env python3
import linkgraph
import os
import sys
import math
import random
import collections
import argparse

def main() :
    # parse options.
    parser = argparse.ArgumentParser();
    parser.add_argument('--log', type=str, dest='logfile', help='path of link status file.', required=True)
    parser.add_argument('--link', type=str, dest='linkfile', help='path of topology discription file.', required=True)
    parser.add_argument('--ttime', type=float, dest='totaltime', help='estemated execution time [ns]', required=True)
    parser.add_argument('--actpwr', type=float, dest='actpwr', help='power consumption on active mode [W]', required=False)
    parser.add_argument('--lpipwr', type=float, dest='lpipwr', help='power consumption on low power mode [W]', required=False)
    opts = parser.parse_args();

    logfile        = opts.logfile
    linkgraph_file = opts.linkfile
    totaltime      = opts.totaltime
    if(opts.lpipwr != None) :
        lpi_power = opts.lpipwr
    else :
        lpi_power = 2.08

    if(opts.actpwr != None) :
        act_power = opts.actpwr
    else :
        act_power = lpi_power+1.36
    
    network = None
    linklist = None
    with open(linkgraph_file, "r") as f:
        network, linklist = linkgraph.mkLinkGraph(f)

    # calculate the time of active mode for each link and port.
    link_communicate = [0.0 for i in range(len(linklist))]
    port_communicate = collections.defaultdict(lambda : 0.0)
    warned = set() # prevent repeated warning

    with open(logfile, "r") as f :
        for i in f :
            i = i.split()
            if not(i[0] in network) :
                if not(i[0] in warned) :
                    print("warning : invalid port {} was used.".format(i[0]), file=sys.stderr)
                    warned.add(i[0])
            else :
                link_id = network[i[0]]

                if link_id :
                    link_communicate[link_id] += float(i[1])
                port_communicate[i[0]] += float(i[1])

                if (float(i[2]) > totaltime) :
                    if link_id :
                        link_communicate[link_id] -= float(i[2]) - totaltime
                    port_communicate[i[0]] -= float(i[2]) - totaltime

    # calculate link-wise power consumption
    linkpower = []
    portpower = {}
    for link_id, port_ids in enumerate(linklist) :
        acttime = link_communicate[link_id]
        '''
        P_portA = (P_act * T_act^(A) + P_lpi * T_lpi^(A)) / T_total  : i.e. up
        P_portB = (P_act * T_act^(B) + P_lpi * T_lpi^(B)) / T_total  : i.e. down
        P_link  = P_portA + P_portB
                = ( P_act * (T_act^(A) + T_act^(B)) + P_lpi * (T_lpi^(A) + T_lpi^(B)) ) / T_total
        '''
        linkpower.append((acttime*act_power + (2*totaltime-acttime)*lpi_power) / totaltime )
    ports = set()
    for i in network :
        ports.add(i)
    for i in ports :
        acttime = port_communicate[i]
        portpower[i] = (acttime*act_power/2 + (totaltime-acttime)*lpi_power/2) / totaltime 

    # calculate switch-wise power consumption from link-wise power consumption
    switchpower = {}
    for link_id, lpower in enumerate(linkpower) :
        u = linklist[link_id][0].split('-')[0]
        v = linklist[link_id][1].split('-')[0]
        if not(u in switchpower) :
            switchpower[u] = 0.0
        if not(v in switchpower) :
            switchpower[v] = 0.0
        switchpower[u] += lpower/2.0
        switchpower[v] += lpower/2.0
    switchpower = list(switchpower.items())
    switchpower.sort()

    # total power consumption is equal to the sum of power consumption of each ports.
    systempower = sum(portpower.values())*2
    #systempower = sum(linkpower)

    for link_id, lpower in enumerate(linkpower) :
        print("link id {}, [{} <--> {}] : {} [W]".format(link_id, linklist[link_id][0], linklist[link_id][1], lpower))
    for i, j in switchpower :
        print("SW/NIC {} : {} [W]".format(i,j))
    for link_id, lpower in enumerate(linkpower) :
        u = linklist[link_id][0].split('-')[0]
        v = linklist[link_id][1].split('-')[0]
        uu = linklist[link_id][0]
        vv = linklist[link_id][1]
        print("onedir link id {}, [{} --> {}] : {} [W]".format(link_id*2, u, v, portpower[uu]))
        print("onedir link id {}, [{} --> {}] : {} [W]".format(link_id*2+1, v, u, portpower[vv]))
    print("system power : {}[W]".format(systempower))
    print("# statistics")

if __name__ == '__main__' :
    main()
