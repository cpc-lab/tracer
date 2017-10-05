#!/usr/bin/env python3
import sys
import collections

def mkLinkGraph(istr) :
    E = collections.defaultdict(lambda : [])
    unconnected = []
    for line in istr :
        line = line[:line.find('#')]
        if len(line) > 0 :
            ss = line.split()
            u  = ss[0][:ss[0].find('-')]
            v  = ss[1]
            if v == "null" :
                unconnected.append(ss[0])
            else :
                if u > v :
                    u, v = v, u
                E[u,v].append(ss[0])
    
    connections = {}
    linklist = []
    cnt = 0
    for key, val in E.items() :
        if len(val) % 2 == 0 and len(val):
            u = val[0][:val[0].find('-')]
            us = [i for i in val if i[:i.find('-')] == u]
            vs = [i for i in val if i[:i.find('-')] != u]
            for i in range(min(len(us), len(vs))) : 
                u = us[i]
                v = vs[i]
                if u > v :
                    u, v = v, u
                connections[u] = cnt
                connections[v] = cnt
                linklist.append((u,v))
                cnt += 1
        else :
            # dragonfly self loop
            if key[0] == key[1] and len(val) == 1 :
                unconnected.append(val[0])
            else:
                print(key, val)
                assert(False)

    for i in unconnected :
        connections[i] = None

    return connections, linklist

if __name__ == '__main__' :
    r = mkLinkGraph(sys.stdin)
    print(r)
