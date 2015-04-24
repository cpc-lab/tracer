#include <ross.h>

#include "codes/codes_mapping.h"
#include "codes/codes.h"
#include "codes/model-net.h"
#include "codes/model-net-method.h"
#include "codes/model-net-lp.h"
#include "codes/net/fattree.h"
#include "sys/file.h"

#define CREDIT_SIZE 8
#define MEAN_PROCESS 1.0

// debugging parameters
#define FATTREE_HELLO 0
#define FATTREE_CONNECTIONS 0
#define FATTREE_MSG 0

#define LP_CONFIG_NM (model_net_lp_config_names[FATTREE])
#define LP_METHOD_NM (model_net_method_names[FATTREE])

static double maxd(double a, double b) { return a < b ? b : a; }

// arrival rate
static double MEAN_INTERVAL=200.0;

typedef struct fattree_param fattree_param;
/* annotation-specific parameters (unannotated entry occurs at the 
 * last index) */
static uint64_t                  num_params = 0;
static fattree_param           * all_params = NULL;
static config_anno_map_t * anno_map   = NULL;

/* global variables for codes mapping */
static char lp_group_name[MAX_NAME_LENGTH];
static char def_group_name[MAX_NAME_LENGTH];
static char *switch_group_name = "FATTREE_SWITCH";
static int def_gname_set = 0;
static int mapping_grp_id, mapping_type_id, mapping_rep_id, mapping_offset;

struct fattree_param
{
  // configuration parameters
  int num_levels;
  int *num_switches; //switches at various levels
  int *switch_radix; //radix of switches are various levels
  double link_bandwidth;/* bandwidth of a wire connecting switches */
  double cn_bandwidth;/* bandwidth of the compute node channels 
                        connected to switch */
  int vc_size; /* buffer size of the link channels */
  int cn_vc_size; /* buffer size of the compute node channels */
  int packet_size; 
  int num_terminals;
  int l1_set_size;
  int l1_term_size;
};

/* handles terminal and switch events like packet generate/send/receive/buffer */
typedef enum event_t event_t;
typedef struct ft_terminal_state ft_terminal_state;
typedef struct switch_state switch_state;

/* fattree compute node data structure */
struct ft_terminal_state
{
  unsigned long long packet_counter;
  // Fattree specific parameters
  unsigned int terminal_id;
  unsigned int switch_id;
  tw_lpid switch_lp;

  // Each terminal will have an input and output channel with the switch
  int vc_occupancy; // NUM_VC
  tw_stime terminal_available_time;
  tw_stime next_credit_available_time;

  char * anno;
  fattree_param *params;
};

/* terminal event type (1-4) */
enum event_t
{
  T_GENERATE=1,
  T_ARRIVE,
  T_SEND,
  T_BUFFER,
  S_SEND,
  S_ARRIVE,
  S_BUFFER,
};

enum last_hop
{
   LINK,
   TERMINAL
};

struct switch_state
{
  unsigned int switch_id;
  int switch_level;
  int radix;
  int num_cons;
  int num_lcons;
  int con_per_lneigh;
  int con_per_uneigh;;
  int start_lneigh;
  int end_lneigh;
  int start_uneigh;

  tw_stime* next_output_available_time;
  tw_stime* next_credit_available_time;
  int* vc_occupancy;
  int* link_traffic;
  tw_lpid *port_connections;

  char * anno;
  fattree_param *params;
};

//decl
void switch_credit_send(switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp);
int ft_get_output_port( switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp, int *out_off );
int get_base_port(switch_state *s, int from_term, int index);


/* returns the fattree switch lp type for lp registration */
static const tw_lptype* fattree_get_switch_lp_type(void);

/* returns the dragonfly message size */
static int fattree_get_msg_sz(void)
{
  return sizeof(fattree_message);
}

static void fattree_read_config(char * anno, fattree_param *p){
  int i;

  configuration_get_value_int(&config, "PARAMS", "num_levels", anno, 
      &p->num_levels);
  if(p->num_levels <= 0) {
    tw_error(TW_LOC, "Too few num_levels, Aborting\n");
  }
  if(p->num_levels > 3) {
    tw_error(TW_LOC, "Too many num_levels, only upto 3 supported Aborting\n");
  }

  p->num_switches = (int *) malloc (p->num_levels * sizeof(int));
  p->switch_radix = (int*) malloc (p->num_levels * sizeof(int));

  char switch_counts_str[MAX_NAME_LENGTH];
  int rc = configuration_get_value(&config, "PARAMS", "switch_count", anno,
      switch_counts_str, MAX_NAME_LENGTH);
  if (rc == 0){
    tw_error(TW_LOC, "couldn't read PARAMS:switch_count");
  }
  char* token;
  token = strtok(switch_counts_str, ",");
  i = 0;
  while(token != NULL)
  {
    sscanf(token, "%d", &p->num_switches[i]);
    if(p->num_switches[i] <= 0)
    {
      tw_error(TW_LOC, "Invalid switch count  specified "
          "(%d at pos %d), exiting... ", p->num_switches[i], i);
    }
    i++;
    token = strtok(NULL,",");
  }

  if(i != p->num_levels) {
    tw_error(TW_LOC, "Not enough switch counts, Aborting\n");
  }
  
  char switch_radix_str[MAX_NAME_LENGTH];
  rc = configuration_get_value(&config, "PARAMS", "switch_radix", anno,
      switch_radix_str, MAX_NAME_LENGTH);
  if (rc == 0){
    tw_error(TW_LOC, "couldn't read PARAMS:switch_radix");
  }
  token = strtok(switch_radix_str, ",");
  i = 0;
  while(token != NULL)
  {
    sscanf(token, "%d", &p->switch_radix[i]);
    if(p->num_switches[i] <= 0)
    {
      tw_error(TW_LOC, "Invalid switch radix  specified "
          "(%d at pos %d), exiting... ", p->switch_radix[i], i);
    }
    i++;
    token = strtok(NULL,",");
  }

  if(i != p->num_levels) {
    tw_error(TW_LOC, "Not enough switch radix, Aborting\n");
  }

  i = 1;
  for(i = 1; i < p->num_levels - 1; i++) {
    if(p->num_switches[i - 1] * p->switch_radix[i - 1] >
       p->num_switches[i] * p->switch_radix[i]) {
      tw_error(TW_LOC, "Not enough switches/radix at level %d for full "
          "bisection bandwidth\n", i);
    }
  }

  if(p->num_switches[i - 1] * p->num_switches[i - 1] > 2 * p->num_switches[i] *
      p->switch_radix[i]) {
    tw_error(TW_LOC, "Not enough switches/radix at level %d for full "
        "bisection bandwidth\n", i);
  }

  configuration_get_value_int(&config, "PARAMS", "vc_size", anno, &p->vc_size);
  if(!p->vc_size) {
    p->vc_size = 2048;
    fprintf(stderr, "Buffer size of global channels not specified, setting to "
        "%d\n", p->vc_size);
  }

  configuration_get_value_int(&config, "PARAMS", "cn_vc_size", anno, 
      &p->cn_vc_size);
  if(!p->cn_vc_size) {
    p->cn_vc_size = 1024;
    fprintf(stderr, "Buffer size of compute node channels not specified, " 
        "setting to %d\n", p->cn_vc_size);
  }

  configuration_get_value_int(&config, "PARAMS", "packet_size", anno,
      &p->packet_size);
  if(!p->packet_size) {
    p->packet_size = 512;
    fprintf(stderr, "Packet size is not specified, setting to %d\n", 
        p->packet_size);
  }

  configuration_get_value_double(&config, "PARAMS", "link_bandwidth", anno, 
      &p->link_bandwidth);
  if(!p->link_bandwidth) {
    p->link_bandwidth = 5;
    fprintf(stderr, "Bandwidth of links is specified, setting to %lf\n", 
        p->link_bandwidth);
  }

  configuration_get_value_double(&config, "PARAMS", "cn_bandwidth", anno, 
      &p->cn_bandwidth);
  if(!p->cn_bandwidth) {
    p->cn_bandwidth = 5;
    fprintf(stderr, "Bandwidth of compute node channels not specified, " 
        "setting to %lf\n", p->cn_bandwidth);
  }

  p->l1_set_size = 0;
  if(p->num_levels == 3) { 
    configuration_get_value_int(&config, "PARAMS", "l1_set_size", anno,
        &p->l1_set_size);
    if(!p->l1_set_size) {
      tw_error(TW_LOC, "l1_set_size not specified, Aborting\n");
    }
  } else {
    p->l1_set_size = p->num_switches[1];
  }
  p->l1_term_size = (p->l1_term_size * (p->switch_radix[1] / 2));
  p->l1_term_size /= (p->switch_radix[0] / 2);
  p->l1_term_size *= (p->switch_radix[0] / 2); 
}

static void fattree_configure(){
  anno_map = (config_anno_map_t *)codes_mapping_get_lp_anno_map(LP_CONFIG_NM);
  assert(anno_map);
  num_params = anno_map->num_annos + (anno_map->has_unanno_lp > 0);
  all_params = malloc(num_params * sizeof(*all_params));

  for (uint64_t i = 0; i < anno_map->num_annos; i++){
    char * anno = anno_map->annotations[i];
    fattree_read_config(anno, &all_params[i]);
  }
  if (anno_map->has_unanno_lp > 0){
    fattree_read_config(NULL, &all_params[anno_map->num_annos]);
  }
}

/* initialize a fattree compute node terminal */
void ft_terminal_init( ft_terminal_state * s, tw_lp * lp )
{
    int i;
    char anno[MAX_NAME_LENGTH];

    if(def_gname_set == 0) {
      def_gname_set = 1;
      codes_mapping_get_lp_info(0, def_group_name, &mapping_grp_id, NULL,
          &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    }

    // Assign the global switch ID
    codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
            &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    if (anno[0] == '\0'){
        s->anno = NULL;
        s->params = &all_params[num_params-1];
    } else {
        s->anno = strdup(anno);
        int id = configuration_get_annotation_index(anno, anno_map);
        s->params = &all_params[id];
    }

   int num_lps = codes_mapping_get_lp_count(lp_group_name, 1, LP_CONFIG_NM,
           s->anno, 0);

   if(num_lps != (s->params->switch_radix[0]/2)) {
     tw_error(TW_LOC, "Number of NICs per repetition has to be equal to "
         "half the radix of leaf level switches %d vs %d\n", num_lps,
          s->params->switch_radix[0]/2);
   }
   s->terminal_id = (mapping_rep_id * num_lps) + mapping_offset;  
   s->switch_id = s->terminal_id / (s->params->switch_radix[0] / 2);
   codes_mapping_get_lp_id(lp_group_name, "fattree_switch", NULL, 1,
           s->switch_id, 0, &s->switch_lp);
   s->terminal_available_time = 0.0;
   s->packet_counter = 0;

#if FATTREE_HELLO
   printf("I am terminal %d (%ld), connected to switch %d\n", s->terminal_id,
       lp->gid, s->switch_id);
#endif
   s->vc_occupancy = 0;

   s->params->num_terminals = codes_mapping_get_lp_count(lp_group_name, 0, 
      LP_CONFIG_NM, s->anno, 0);
   return;
}

/* sets up the switch */
void switch_init(switch_state * r, tw_lp * lp)
{
  char anno[MAX_NAME_LENGTH];
  int num_terminals, num_lps;

  if(def_gname_set == 0) {
    def_gname_set = 1;
    codes_mapping_get_lp_info(0, def_group_name, &mapping_grp_id, NULL,
        &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);
    num_terminals = codes_mapping_get_lp_count(def_group_name, 0, 
      LP_CONFIG_NM, anno, 0);
    num_lps = codes_mapping_get_lp_count(def_group_name, 1, LP_CONFIG_NM,
           anno, 0);
    /*if(num_lps != (r->params->switch_radix[0]/2)) {
      tw_error(TW_LOC, "Number of NICs per repetition has to be equal to "
          "half the radix of leaf level switches\n");
    }*/
  }

  codes_mapping_get_lp_info(lp->gid, lp_group_name, &mapping_grp_id, NULL,
      &mapping_type_id, anno, &mapping_rep_id, &mapping_offset);

  if (anno[0] == '\0'){
    r->anno = NULL;
    r->params = &all_params[num_params-1];
  }
  else{
    r->anno = strdup(anno);
    int id = configuration_get_annotation_index(anno, anno_map);
    r->params = &all_params[id];
  }

  // shorthand
  fattree_param *p = r->params;

  r->switch_id = mapping_rep_id + mapping_offset;
  p->num_terminals = num_terminals;

  if(strcmp(def_group_name, lp_group_name) != 0) {
    r->switch_id += p->num_switches[0];
  }
  
  int sum = 0;
  for(int level = 0; level < p->num_levels; level++) {
    sum += p->num_switches[level];
    if(r->switch_id < sum) {
      r->switch_level = level;
      break;
    }
  }

  r->radix = p->switch_radix[r->switch_level];

  r->next_output_available_time = (tw_stime*) malloc (r->radix * 
      sizeof(tw_stime));
  r->next_credit_available_time = (tw_stime*) malloc (r->radix * 
      sizeof(tw_stime));
  r->vc_occupancy = (int*) malloc (r->radix * sizeof(int));
  r->link_traffic = (int*) malloc (r->radix * sizeof(int));
  r->port_connections = (tw_lpid*) malloc (r->radix * sizeof(tw_lpid));

  for(int i = 0; i < r->radix; i++)
  {
    // Set credit & switch occupancy
    r->next_output_available_time[i] = 0;
    r->next_credit_available_time[i] = 0;
    r->vc_occupancy[i] = 0;
    r->link_traffic[i] = 0;
  }

  //set lps connected to each port
  r->num_cons = 0;
  r->num_lcons = 0;
#if FATTREE_HELLO
  printf("I am switch %d (%d), level %d, radix %d\n", r->switch_id,
    lp->gid, r->switch_level, r->radix);
#endif
  //if at level 0, first half ports go to terminals
  if(r->switch_level == 0) {
    int start_terminal = r->switch_id * (p->switch_radix[0] / 2);
    int end_terminal = start_terminal + (p->switch_radix[0] / 2);
    for(int term = start_terminal; term < end_terminal; term++) {
      tw_lpid nextTerm;
      int rep = term / (p->switch_radix[0] / 2);
      int off = term % (p->switch_radix[0] / 2);
      codes_mapping_get_lp_id(def_group_name, LP_CONFIG_NM, NULL, 1,
          rep, off, &nextTerm);
      r->port_connections[r->num_cons++] = nextTerm;
      r->num_lcons++;
#if FATTREE_DEBUG
      printf("I am switch %d, connect to terminal %d (%d) at port %d\n",
          r->switch_id, term, nextTerm, r->num_cons - 1);
#endif
    }
    r->start_lneigh = start_terminal;
    r->end_lneigh = end_terminal;
    r->con_per_lneigh = 1;
    assert(r->num_lcons == (r->radix / 2));
    int l1_set;
    if(p->num_levels == 2) {
      l1_set = 0;
    } else {
      int l1_width = p->l1_set_size * (p->switch_radix[1] / 2); 
      l1_set = (r->switch_id * (r->radix / 2)) / l1_width;
    }
    int l1_base = l1_set * p->l1_set_size;
    int con_per_l1 = (r->radix / 2) / p->l1_set_size;
    r->start_uneigh = p->num_switches[0] + l1_base;
    r->con_per_uneigh = con_per_l1;
    for(int l1 = 0; l1 < p->l1_set_size; l1++) {
      tw_lpid nextTerm;
      codes_mapping_get_lp_id(switch_group_name, "fattree_switch", NULL, 1,
          l1_base, 0, &nextTerm);
      for(int con = 0; con < con_per_l1; con++) {
        r->port_connections[r->num_cons++] = nextTerm;
#if FATTREE_DEBUG
      printf("I am switch %d, connect to upper switch %d (%d) at port %d\n",
          r->switch_id, l1_base, nextTerm, r->num_cons - 1);
#endif
      }
      l1_base++;
    }
  } else if (r->switch_level == 1) {
    int l0_set_size, l0_base;
    if(p->num_levels == 2) {
      l0_set_size = p->num_switches[0];
      l0_base = 0;
      r->start_lneigh = 0;
      r->end_lneigh = p->num_switches[0];
    } else {
      l0_set_size = (p->l1_set_size * (p->switch_radix[1] / 2)) /
        (p->switch_radix[0] / 2); 
      l0_base = ((r->switch_id - p->num_switches[0]) / p->l1_set_size) *
        l0_set_size;
      r->start_lneigh = l0_base;
      r->end_lneigh = l0_base + l0_set_size;
    }
    int con_per_l0 = (p->switch_radix[0] / 2) / p->l1_set_size;
    r->con_per_lneigh = con_per_l0;
    for(int l0 = 0; l0 < l0_set_size; l0++) {
      tw_lpid nextTerm;
      codes_mapping_get_lp_id(def_group_name, "fattree_switch", NULL, 1,
          l0_base, 0, &nextTerm);
      for(int con = 0; con < con_per_l0; con++) {
        r->port_connections[r->num_cons++] = nextTerm;
        r->num_lcons++;
#if FATTREE_DEBUG
        printf("I am switch %d, connect to switch %d (%d) at port %d\n",
            r->switch_id, l0_base, nextTerm, r->num_cons - 1);
#endif
      }
      l0_base++;
    }
    if(p->num_levels == 3) {
      int l2_base = p->num_switches[1];
      int con_per_l2 = r->num_lcons / p->num_switches[2];
      r->start_uneigh = p->num_switches[0] + l2_base;
      r->con_per_uneigh = con_per_l2;
      for(int l2 = 0; l2 < p->num_switches[2]; l2++) {
        tw_lpid nextTerm;
        codes_mapping_get_lp_id(switch_group_name, "fattree_switch", NULL, 1,
            l2_base, 0, &nextTerm);
        for(int con = 0; con < con_per_l2; con++) {
          r->port_connections[r->num_cons++] = nextTerm;
#if FATTREE_DEBUG
          printf("I am switch %d, connect to upper switch %d (%d) at port %d\n",
              r->switch_id, l2_base, nextTerm, r->num_cons - 1);
#endif
        }
        l2_base++;
      }
    }
  } else {
    int con_per_l1 = ((p->num_switches[0] * (p->switch_radix[0] / 2)) /
        p->num_switches[1])/ p->num_switches[2];
    r->con_per_lneigh = con_per_l1;
    r->start_lneigh = p->num_switches[0];
    r->end_lneigh = r->start_lneigh + p->num_switches[1];
    for(int l1 = 0; l1 < p->num_switches[1]; l1++) {
      tw_lpid nextTerm;
      codes_mapping_get_lp_id(switch_group_name, "fattree_switch", NULL, 1,
          l1, 0, &nextTerm);
      for(int con = 0; con < con_per_l1; con++) {
        r->port_connections[r->num_cons++] = nextTerm;
        r->num_lcons++;
#if FATTREE_DEBUG
        printf("I am switch %d, connect to  switch %d (%d) at port %d\n",
            r->switch_id, l1, nextTerm, r->num_cons - 1);
#endif
      }
    }
  }
  return;
}	

/* empty for now.. */
static void fattree_report_stats() { }

/* fattree packet event */
static tw_stime fattree_packet_event(char* category, tw_lpid final_dest_lp, 
    uint64_t packet_size, int is_pull, uint64_t pull_size, tw_stime offset, 
    const mn_sched_params *sched_params, int remote_event_size, 
    const void* remote_event, int self_event_size, const void* self_event, 
    tw_lpid src_lp, tw_lp *sender, int is_last_pckt) {

  tw_event * e_new;
  tw_stime xfer_to_nic_time;
  fattree_message * msg;
  char* tmp_ptr;

  xfer_to_nic_time = codes_local_latency(sender);     
  e_new = model_net_method_event_new(sender->gid, xfer_to_nic_time + offset,
      sender, FATTREE, (void**)&msg, (void**)&tmp_ptr);
  strcpy(msg->category, category);
  msg->final_dest_gid = final_dest_lp;
  msg->sender_lp = src_lp;
  msg->packet_size = packet_size;
  msg->remote_event_size_bytes = 0;
  msg->local_event_size_bytes = 0;
  msg->type = T_GENERATE;

  if(is_last_pckt) /* Its the last packet so pass in remote and local event information*/
  {
    if(remote_event_size > 0)
    {
      msg->remote_event_size_bytes = remote_event_size;
      memcpy(tmp_ptr, remote_event, remote_event_size);
      tmp_ptr += remote_event_size;
    }
    if(self_event_size > 0)
    {
      msg->local_event_size_bytes = self_event_size;
      memcpy(tmp_ptr, self_event, self_event_size);
      tmp_ptr += self_event_size;
    }
  }
  tw_event_send(e_new);
  return xfer_to_nic_time;
}

/* fattree packet event reverse handler */
static void fattree_packet_event_rc(tw_lp *sender)
{
  codes_local_latency_reverse(sender);
  return;
}

/* generates packet at the current fattree compute node */
void ft_packet_generate(ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  fattree_param *p = s->params;
  
  codes_mapping_get_lp_info(msg->final_dest_gid, lp_group_name, &mapping_grp_id,
      NULL, &mapping_type_id, NULL, &mapping_rep_id, &mapping_offset);
  msg->dest_num = (mapping_rep_id * (p->switch_radix[0] / 2)) +  mapping_offset;

  tw_stime ts;
  tw_event *e;
  fattree_message *m;
  int i, total_event_size;
  msg->packet_ID = lp->gid + g_tw_nlp * s->packet_counter + 
      tw_rand_integer(lp->rng, 0, lp->gid + g_tw_nlp * s->packet_counter);
        
  ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, MEAN_INTERVAL/200);
  int chan = -1;
  if(s->vc_occupancy < p->cn_vc_size)
  {
    chan = 0;
  }

  void * m_data;
  e = model_net_method_event_new(lp->gid, ts, lp, FATTREE, (void**)&m, 
      &m_data);
  memcpy(m, msg, sizeof(fattree_message));
  m->dest_num = msg->dest_num;
  void * m_data_src = model_net_method_get_edata(FATTREE, msg);
  if (msg->remote_event_size_bytes){
    memcpy(m_data, m_data_src, msg->remote_event_size_bytes);
  }
  if (msg->local_event_size_bytes){ 
    memcpy((char*)m_data + msg->remote_event_size_bytes,
        (char*)m_data_src + msg->remote_event_size_bytes,
        msg->local_event_size_bytes);
  }
  m->saved_vc = chan;

  if(chan != -1) // If the input queue is available
  {
    // Send the packet out
    m->type = T_SEND;
    tw_event_send(e);
  } else {
    printf("\n Exceeded queue size, exitting %d", s->vc_occupancy);
    MPI_Abort(MPI_COMM_WORLD, 1);
  } 
  return;
}

/* sends the packet from the compute node to the attached switch */
void ft_packet_send(ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  tw_stime ts;
  tw_event *e;
  fattree_message *m;
  
  /* Route the packet to its source switch */ 
  int vc = msg->saved_vc;

  //  Each packet is broken into chunks and then sent over the channel
  msg->saved_available_time = s->terminal_available_time;
  double head_delay = (1 / s->params->cn_bandwidth) * s->params->packet_size;
  ts = head_delay + tw_rand_exponential(lp->rng, (double)head_delay / 200);
  s->terminal_available_time = maxd(s->terminal_available_time, tw_now(lp));
  s->terminal_available_time += ts;

  // we are sending an event to the switch, so no method_event here
  e = tw_event_new(s->switch_lp, s->terminal_available_time - tw_now(lp), lp);

  m = tw_event_data(e);
  memcpy(m, msg, sizeof(fattree_message));
  if (msg->remote_event_size_bytes){
    memcpy(m+1, model_net_method_get_edata(FATTREE, msg),
        msg->remote_event_size_bytes);
  }

  m->type = S_ARRIVE;
  m->src_terminal_id = lp->gid;
  m->intm_id = s->terminal_id;
  m->saved_vc = vc;
  m->saved_off = 0; //we only have one connection to the terminal NIC
  m->local_event_size_bytes = 0;
  m->last_hop = TERMINAL;
  tw_event_send(e);
      
  // now that message is sent, issue an "idle" event to tell the scheduler
  // when I'm next available
  model_net_method_idle_event(codes_local_latency(lp) +
      s->terminal_available_time - tw_now(lp), 0, lp);

  /* local completion message */
  if(msg->local_event_size_bytes > 0)
  {
    tw_event* e_new;
    fattree_message* m_new;
    void* local_event = (char*) model_net_method_get_edata(FATTREE, msg) + 
      msg->remote_event_size_bytes;
    ts = g_tw_lookahead + (1 / s->params->cn_bandwidth) * 
        msg->local_event_size_bytes;
    e_new = tw_event_new(msg->sender_lp, ts, lp);
    m_new = tw_event_data(e_new);
    memcpy(m_new, local_event, msg->local_event_size_bytes);
    tw_event_send(e_new);
  }
   
  s->packet_counter++;
  s->vc_occupancy++;
  
  return;
}

/* Packet arrives at the switch and a credit is sent back to the sending 
 * terminal/switch */
void switch_packet_receive( switch_state * s, tw_bf * bf, 
    fattree_message * msg, tw_lp * lp ) {

  tw_event *e;
  fattree_message *m;
  tw_stime ts;

  ts = g_tw_lookahead + 0.1 + tw_rand_exponential(lp->rng, 
      (double)MEAN_INTERVAL/200);

  // switch self message - no need for method_event
  e = tw_event_new(lp->gid, ts, lp);
  m = tw_event_data(e);
  memcpy(m, msg, sizeof(fattree_message) + msg->remote_event_size_bytes);
  m->type = S_SEND;
  switch_credit_send(s, bf, msg, lp);
  tw_event_send(e);  
  return;
}

/* When a packet is sent from the current switch and a buffer slot 
 * becomes available, a credit is sent back to schedule another packet 
 * event */
void switch_credit_send(switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  tw_event * buf_e;
  tw_stime ts;
  fattree_message * buf_msg;

  int dest = 0, credit_delay = 0, type = S_BUFFER;
  int is_terminal = 0;

  fattree_param *p = s->params;
  // Notify sender terminal about available buffer space
  if(msg->last_hop == TERMINAL) {
    dest = msg->src_terminal_id;
    //determine the time in ns to transfer the credit
    credit_delay = (1 / p->cn_bandwidth) * CREDIT_SIZE;
    type = T_BUFFER;
    is_terminal = 1;
  } else if(msg->last_hop == LINK) {
    dest = msg->intm_lp_id;
    credit_delay = (1 / p->link_bandwidth) * CREDIT_SIZE;
  } else tw_error(TW_LOC, "\n Invalid message type");

  // Assume it takes 0.1 ns of serialization latency for processing the 
  // credits in the queue
  int output_port = msg->saved_off; //src used this offset, so I have to
  output_port += get_base_port(s, is_terminal, msg->intm_id);

  msg->saved_available_time = s->next_credit_available_time[output_port];
  s->next_credit_available_time[output_port] = maxd(tw_now(lp), 
      s->next_credit_available_time[output_port]);
  ts = credit_delay + 0.1 + tw_rand_exponential(lp->rng, 
      (double)credit_delay / 1000);
	
  s->next_credit_available_time[output_port] += ts;
  if (is_terminal) {
    buf_e = model_net_method_event_new(dest, 
        s->next_credit_available_time[output_port] - tw_now(lp), lp,
        FATTREE, (void**)&buf_msg, NULL);
  } else {
    buf_e = tw_event_new(dest, s->next_credit_available_time[output_port] - 
      tw_now(lp) , lp);
    buf_msg = tw_event_data(buf_e);
  }

  buf_msg->vc_index = msg->saved_vc; //the port src used to send me this data
  buf_msg->type = type;
  buf_msg->last_hop = msg->last_hop;
  buf_msg->packet_ID = msg->packet_ID;

  msg->saved_vc = output_port;
  tw_event_send(buf_e);
  return;
}

/* update the compute node-switch channel buffer */
void ft_terminal_buf_update(ft_terminal_state * s, tw_bf * bf,
    fattree_message * msg, tw_lp * lp) {

    s->vc_occupancy--;
    return;
}

void switch_buf_update(switch_state * s, tw_bf * bf, 
    fattree_message * msg, tw_lp * lp) {

    int msg_indx = msg->vc_index;
    s->vc_occupancy[msg_indx]--;
    return;
}

/* packet arrives at the destination terminal */
void ft_packet_arrive(ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  // Packet arrives and accumulate # queued
  // Find a queue with an empty buffer slot
  tw_event * e, * buf_e;
  fattree_message * m, * buf_msg;
  tw_stime ts;

  // Trigger an event on receiving server
  if(msg->remote_event_size_bytes)
  {
    void * tmp_ptr = model_net_method_get_edata(FATTREE, msg);
    ts = g_tw_lookahead + 0.1 + (1 / s->params->cn_bandwidth) *
      msg->remote_event_size_bytes;
    e = tw_event_new(msg->final_dest_gid, ts, lp);
    m = tw_event_data(e);
    memcpy(m, tmp_ptr, msg->remote_event_size_bytes);
    tw_event_send(e); 
  }

  int credit_delay = (1 / s->params->cn_bandwidth) * CREDIT_SIZE;
  ts = credit_delay + 0.1 + tw_rand_exponential(lp->rng, credit_delay / 1000);
  
  msg->saved_credit_time = s->next_credit_available_time;
  s->next_credit_available_time = maxd(s->next_credit_available_time, tw_now(lp));
  s->next_credit_available_time += ts;

  // no method_event here - message going to switch
  buf_e = tw_event_new(s->switch_lp, s->next_credit_available_time - tw_now(lp), 
      lp);
  buf_msg = tw_event_data(buf_e);
  buf_msg->vc_index = msg->saved_vc;
  buf_msg->type = S_BUFFER;
  buf_msg->packet_ID = msg->packet_ID;
  buf_msg->last_hop = TERMINAL;
  tw_event_send(buf_e);
  return;
}

/* routes the current packet to the next stop */
void switch_packet_send( switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  tw_stime ts;
  tw_event *e;
  fattree_message *m;

  int next_stop = -1, output_port = -1, out_off = 0;
  double bandwidth = s->params->link_bandwidth;

  output_port = ft_get_output_port(s, bf, msg, lp, &out_off);
  next_stop = s->port_connections[output_port];

  int max_vc_size = s->params->vc_size;
  int to_terminal = 0;

  //If going to terminal, use a different max
  if(s->switch_level == 0 && output_port < s->num_lcons) {
    max_vc_size = s->params->cn_vc_size;
    to_terminal = 1;
    bandwidth = s->params->cn_bandwidth;
  }

  if(s->vc_occupancy[output_port] >= max_vc_size) {
    printf("\n Switch %d (gid %ld) buffers overflowed for port %d, occ %d ",
        s->switch_id, (long int) lp->gid, output_port,
        s->vc_occupancy[output_port]);
    MPI_Abort(MPI_COMM_WORLD, 1);
    return;
  }

  msg->saved_available_time = s->next_output_available_time[output_port];
  ts = g_tw_lookahead + 0.1 + (( 1 / bandwidth) * s->params->packet_size) + 
      tw_rand_exponential(lp->rng, (double)s->params->packet_size/200);

  s->next_output_available_time[output_port] = 
    maxd(s->next_output_available_time[output_port], tw_now(lp));
  s->next_output_available_time[output_port] += ts;

  // dest can be a switch or a terminal, so we must check
  void * m_data;
  if (to_terminal) {
    e = model_net_method_event_new(next_stop, 
        s->next_output_available_time[output_port] - tw_now(lp), lp,
        DRAGONFLY, (void**)&m, &m_data);
  } else {
      e = tw_event_new(next_stop, s->next_output_available_time[output_port] 
        - tw_now(lp), lp);
      m = tw_event_data(e);
      m_data = m + 1;
  }

  memcpy(m, msg, sizeof(fattree_message));
  if (msg->remote_event_size_bytes){
      memcpy(m_data, msg + 1, msg->remote_event_size_bytes);
  }

  m->last_hop = LINK;
  m->saved_vc = output_port;
  m->saved_off = out_off;;
  m->intm_lp_id = lp->gid;
  m->intm_id = s->switch_id;
  s->vc_occupancy[output_port]++;
  s->link_traffic[output_port] += msg->packet_size;

  msg->saved_vc = output_port; //for reverse handler

  /* Determine the event type. If the packet has arrived at the final destination
     switch then it should arrive at the destination terminal next. */
  if(to_terminal) {
    m->type = T_ARRIVE;
  } else {
    /* The packet has to be sent to another switch */
    m->type = S_ARRIVE;
  }
  tw_event_send(e);
  return;
}

/* gets the output port corresponding to the next stop of the message */
int ft_get_output_port( switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp, int *out_off) {

  int outport = -1;
  int start_port, end_port;
  fattree_param *p = s->params;

  if(s->switch_level == 0) {
    //message for a terminal node
    if(msg->dest_num >= s->start_lneigh && msg->dest_num < s->end_lneigh) {
      outport = msg->dest_num - s->start_lneigh;
      *out_off = 0;
      return outport;
    } else { //go up the least congested path
      start_port = s->num_lcons;
      end_port = s->num_cons;
    }
  } else if(s->switch_level == 1) {
    int dest_switch_id = msg->dest_num / (p->switch_radix[0] / 2);
    //if only two level or packet going down, send to the right switch
    if(p->num_levels == 2 || (dest_switch_id >= s->start_lneigh && 
      dest_switch_id < s->end_lneigh)) {
      start_port = (dest_switch_id - s->start_lneigh) * s->con_per_lneigh;
      end_port = outport + s->con_per_lneigh;
    } else {
      start_port = s->num_lcons;
      end_port = s->num_lcons;
    }
  } else { //switch level 2
    int dest_l1_group = msg->dest_num / p->l1_term_size;
    start_port = dest_l1_group * p->l1_set_size * s->con_per_lneigh;
    end_port = outport + (p->l1_set_size * s->con_per_lneigh);
  }

  outport = start_port;
  int load = s->vc_occupancy[outport];
  if(load != 0) {
    for(int port = start_port + 1; port < end_port; port++) {
      if(s->vc_occupancy[port] < load) {
        load = s->vc_occupancy[port];
        outport = port;
        if(load == 0) break;
      }
    }
  }
  assert(outport != -1);
  if(outport < s->num_lcons) {
    *out_off = outport % s->con_per_lneigh;
  } else {
    *out_off = (outport - s->num_lcons) % s->con_per_uneigh;
  }
  return outport;
}

int get_base_port(switch_state *s, int from_term, int index) {
  int return_port;
  if(from_term || index < s->switch_id) {
    return_port += ((index - s->start_lneigh) * s->con_per_lneigh);
  } else {
    return_port = s->num_lcons;
    return_port += ((index - s->start_uneigh) * s->con_per_uneigh);
  }
  return return_port;
}

void ft_terminal_event( ft_terminal_state * s, tw_bf * bf, fattree_message * msg,
		tw_lp * lp ) {

  *(int *)bf = (int)0;
  switch(msg->type) {

    case T_GENERATE:
      ft_packet_generate(s, bf, msg, lp);
      break;

    case T_ARRIVE:
      ft_packet_arrive(s, bf, msg, lp);
      break;

    case T_SEND:
      ft_packet_send(s, bf, msg, lp);
      break;

    case T_BUFFER:
      ft_terminal_buf_update(s, bf, msg, lp);
      break;

    default:
      printf("\n LP %d Terminal message type not supported %d ", (int)lp->gid, msg->type);
  }
}

void fattree_terminal_final( ft_terminal_state * s, tw_lp * lp ) { }

void fattree_switch_final(switch_state * s, tw_lp * lp) {
  char *stats_file = getenv("TRACER_LINK_FILE");
  if(stats_file != NULL) {
    FILE *fout = fopen(stats_file, "a");
    fattree_param *p = s->params;
    int result = flock(fileno(fout), LOCK_EX);
    fprintf(fout, "%d %d", s->switch_id, s->switch_level);
    for(int d = 0; d < s->num_cons; d++) {
      fprintf(fout, "%d ", s->link_traffic[d]);
    }
    fprintf(fout, "\n");
    result = flock(fileno(fout), LOCK_UN);
    fclose(fout);
  }
}

/* Update the buffer space associated with this switch LP */
void switch_event(switch_state * s, tw_bf * bf, fattree_message * msg,
    tw_lp * lp) {

  *(int *)bf = (int)0;
  switch(msg->type) {

    case S_SEND: 
      switch_packet_send(s, bf, msg, lp);
      break;

    case S_ARRIVE: 
      switch_packet_receive(s, bf, msg, lp);
      break;

    case S_BUFFER:
      switch_buf_update(s, bf, msg, lp);
      break;

    default:
      printf("\n (%lf) [Switch %d] Switch Message type not supported %d " 
        "dest terminal id %d packet ID %d ", tw_now(lp), (int)lp->gid, 
        msg->type, (int)msg->dest_num, (int)msg->packet_ID);
      break;
  }	   
}

/* Reverse computation handler for a terminal event */
void ft_terminal_rc_event_handler(ft_terminal_state * s, tw_bf * bf,
    fattree_message * msg, tw_lp * lp) {

  switch(msg->type) {

    case T_GENERATE:
      {
        tw_rand_reverse_unif(lp->rng);
        tw_rand_reverse_unif(lp->rng);
      }
      break;

    case T_SEND:
      {
        s->terminal_available_time = msg->saved_available_time;
        tw_rand_reverse_unif(lp->rng);
        s->vc_occupancy--;
        s->packet_counter--;
        codes_local_latency_reverse(lp);
      }
      break;

    case T_ARRIVE:
      {
        tw_rand_reverse_unif(lp->rng);
        s->next_credit_available_time = msg->saved_credit_time;
      }
      break;

    case T_BUFFER:
      {
        s->vc_occupancy++;
      }  
      break;
  }
}

/* Reverse computation handler for a switch event */
void switch_rc_event_handler(switch_state * s, tw_bf * bf,
    fattree_message * msg, tw_lp * lp) {

  switch(msg->type) {
    case S_SEND:
      {
        tw_rand_reverse_unif(lp->rng);
        int output_port = msg->saved_vc;
        s->next_output_available_time[output_port] = 
          msg->saved_available_time;
        s->vc_occupancy[output_port]--;
        s->link_traffic[output_port] -= msg->packet_size;
      }
      break;

    case S_ARRIVE:
      {
        tw_rand_reverse_unif(lp->rng);
        int output_port = msg->saved_vc;
        s->next_credit_available_time[output_port] = 
          msg->saved_available_time;
        tw_rand_reverse_unif(lp->rng);
      }
      break;

    case S_BUFFER:
      {
        int msg_indx = msg->vc_index;
        s->vc_occupancy[msg_indx]++;
      }
      break;

  }
}
/* dragonfly compute node and switch LP types */
tw_lptype fattree_lps[] =
{
  // Terminal handling functions
  {
    (init_f)ft_terminal_init,
    (pre_run_f) NULL,
    (event_f) ft_terminal_event,
    (revent_f) ft_terminal_rc_event_handler,
    (final_f) fattree_terminal_final,
    (map_f) codes_mapping,
    sizeof(ft_terminal_state)
  },
  {
    (init_f) switch_init,
    (pre_run_f) NULL,
    (event_f) switch_event,
    (revent_f) switch_rc_event_handler,
    (final_f) fattree_switch_final,
    (map_f) codes_mapping,
    sizeof(switch_state),
  },
  {0},
};

/* returns the fattree lp type for lp registration */
static const tw_lptype* fattree_get_cn_lp_type(void)
{
  return(&fattree_lps[0]);
}
static const tw_lptype* fattree_get_switch_lp_type(void)
{
  return(&fattree_lps[1]);
}          

struct model_net_method fattree_method =
{
  .mn_configure = fattree_configure,
  .model_net_method_packet_event = fattree_packet_event,
  .model_net_method_packet_event_rc = fattree_packet_event_rc,
  .model_net_method_recv_msg_event = NULL,
  .model_net_method_recv_msg_event_rc = NULL,
  .mn_get_lp_type = fattree_get_cn_lp_type,
  .mn_get_msg_sz = fattree_get_msg_sz,
  .mn_report_stats = fattree_report_stats,
  .model_net_method_find_local_device = NULL,
  .mn_collective_call = NULL,
  .mn_collective_call_rc = NULL
};
