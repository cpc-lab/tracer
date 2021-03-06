//////////////////////////////////////////////////////////////////////////////
// Copyright (c) 2015, Lawrence Livermore National Security, LLC.
// Produced at the Lawrence Livermore National Laboratory.
//
// Written by:
//     Nikhil Jain <nikhil.jain@acm.org>
//     Bilge Acun <acun2@illinois.edu>
//     Abhinav Bhatele <bhatele@llnl.gov>
//
// LLNL-CODE-681378. All rights reserved.
//
// This file is part of TraceR. For details, see:
// https://github.com/LLNL/tracer
// Please also read the LICENSE file for our notice and the LGPL.
//////////////////////////////////////////////////////////////////////////////

#include <cstdio>
#include <cstdlib>
#include <string.h>
#include <vector>

using namespace std;

int main(int argc, char**argv) {
  FILE *binout = fopen(argv[1], "wb");
  int jobid = 0;
  FILE* out_files;

  if(argc < 6) {
    printf("Correct usage: %s <global_file_name> <sum of ranks used by all jobs> <nodes per router> <servers/cores per node> [optional <nodes within routers to skip after>]\n",
        argv[0]);
    exit(1);
  }

  char dFILE[256];
  sprintf(dFILE, "%s%d", "job", 0);
  out_files = fopen(dFILE, "wb");

  int numAllocCores = atoi(argv[2]);
  int rr_group = atoi(argv[3]);
  int rr = atoi(argv[4]);
  int skip = rr_group;
  if(argc > 5) {
    skip = atoi(argv[5]);
  }
  int local_rank = 0;
  
  for(int i = 0; ; i += rr_group*rr) {
    for(int j = 0; j < rr_group; j++) {
      if(j == skip) {
        break;
      }
      for(int k = 0; k < rr; k++) {
        int global_rank = i + j + k * rr_group;
        fwrite(&global_rank, sizeof(int), 1, binout);
        fwrite(&local_rank, sizeof(int), 1, binout);
        fwrite(&jobid, sizeof(int), 1, binout);
        fwrite(&global_rank, sizeof(int), 1, out_files);
#if PRINT_MAP
        printf("%d %d %d\n", global_rank, local_rank, jobid);
#endif
        local_rank++;
        if(local_rank == numAllocCores) {
          break;
        }
      }
      if(local_rank == numAllocCores) {
        break;
      }
    }
    if(local_rank == numAllocCores) {
      break;
    }
  }

  fclose(binout);
  fclose(out_files);
}
