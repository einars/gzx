/*
  GZX - George's ZX Spectrum Emulator
  tape support
*/


#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "gzx.h"
#include "intdef.h"
#include "memio.h"
#include "strutil.h"
#include "zx_tape.h"
#include "z80.h"
#include "zxt_fif.h"

/* 0=LOW 1=HI */

extern tfr_t tfr_tap;	/* defined in zxt_tap.c */
extern tfr_t tfr_wav;	/* defined in zxt_wav.c */
extern tfr_t tfr_tzx;	/* defined in zxt_tzx.c */

static int cur_level;
static int tape_delta_t;

static int tape_playing,tape_paused;

/* currently playing block */
static int pb_type;

static tfr_t *tfr;

static void (*gsmp)(void);

static void gs_data(void);
static void gs_voice(void);
static void gs_tones(void);

static void gs_data_newblock(void);
static void gs_voice_newblock(void);
static void gs_tones_newblock(void);

/*** tone syntetizer ***/
static int syn_cnt;
static int syn_pulse_len;
static int syn_pulses_left;
static int syn_tones_left;
static int syn_t_pulses[20];
static int syn_t_pulse_len[20];

static void syn_reset(void) {
  syn_pulses_left=0;
  syn_tones_left=0;
  syn_cnt=syn_pulse_len=0;
}

/* generates a new sample. returns 0=success, 1=no more samples */
static int syn_gs(void) {
  if(syn_cnt>=syn_pulse_len) { /* next pulse */
    syn_cnt-=syn_pulse_len;
    if(syn_pulses_left==0) { /* next tone */
      if(syn_tones_left==0) { /* no more samples */
        return 1;
      }
      syn_tones_left--;
      syn_pulse_len=syn_t_pulse_len[syn_tones_left];
      syn_pulses_left=syn_t_pulses[syn_tones_left];
//      fprintf(logfi,"tone %d: num=%d len=%d\n",syn_tones_left+1,
//        syn_pulses_left,syn_pulse_len);
    }
    cur_level^=1;
//    if(cur_level) fprintf(logfi,"edge L->H\n");
//      else fprintf(logfi,"edge H->L\n");
    syn_pulses_left--;
  }
  syn_cnt+=tape_delta_t;
  return 0;
}

/*** tone block getsmp ***/

static int tone_new;

static void gs_tones_newblock(void) {
  tfr->open_block();
  syn_reset();
  tone_new=1;
}

static void gs_tones(void) {
  if(tone_new) {
    if(tfr->b_tones_gettone(&syn_t_pulses[0],&syn_t_pulse_len[0])) {
      /* end of block */
      pb_type=-1;
      tfr->close_block();
      return;
    }
    syn_tones_left=1;
  }
  tone_new=syn_gs();
}

/*** data block getsmp ***/

static tb_data_info_t b_data_info;
static int data_state; /* 0=program leadin 
                          1=playing leadin
                          2=program data
                          3=play data
                          4=play pause */
static int data_cnt;   /* for playing the pause */

static void gs_data_newblock(void) {
  tfr->get_b_data_info(&b_data_info);
  tfr->open_block();
  syn_reset();
  data_state=b_data_info.has_leadin ? 0 : 1;
  data_cnt=0;
  
  /* zero-time block? */
  if(b_data_info.data_bytes==0 && !b_data_info.has_leadin) {
    pb_type=-1;
    tfr->close_block();
  }
}

static void gs_data(void) {
  u8 b;
  int i;
  
//  fprintf(logfi,"ds:%d\n",data_state);fflush(logfi);
  
  if(data_state==0) { /* program leadin */
//    fprintf(logfi,"synthetizing leadin\n");fflush(logfi);
	  
    syn_reset();
	  
    syn_tones_left=3;
    /* pilot */
    syn_t_pulses   [2]=b_data_info.pilot_pulses;
    syn_t_pulse_len[2]=b_data_info.pilot_len;
    printf("pilot pulse len: %d T\n",b_data_info.pilot_len);
    /* sync 1 */
    syn_t_pulses   [1]=1;
    syn_t_pulse_len[1]=b_data_info.sync1_len;
    printf("sync1 pulse len: %d T\n",b_data_info.sync1_len);
    /* sync 2 */
    syn_t_pulses   [0]=1;
    syn_t_pulse_len[0]=b_data_info.sync2_len;
    printf("sync2 pulse len: %d T\n",b_data_info.sync2_len);
      
    data_state=1;
  }
  
  if(data_state==1) { /* play leadin */
    if(syn_gs()) data_state=2;
  }
  
  if(data_state==2) { /* program data */
    if(tfr->b_data_getbytes(1,&b)) {
      data_state=4;
      cur_level^=1;
//      fprintf(logfi,"done with data. setting level=%d\n",cur_level);
    } else {
//      fprintf(logfi,"synthetizing a byte\n");fflush(logfi);
      syn_reset();
      syn_tones_left=8;
      for(i=7;i>=0;i--) {
        syn_t_pulses   [i]=2;
        syn_t_pulse_len[i]=(b&0x80) ? b_data_info.one_len 
                                    : b_data_info.zero_len;
	b<<=1;
      }
      data_state=3;
    }
  }
  
  if(data_state==3) { /* play data */
    if(syn_gs()) data_state=2;
  }
  
  if(data_state==4) { /* play pause */
//    fprintf(logfi,"synt. pause\n");
    data_cnt+=tape_delta_t;
    
    if(data_cnt>=b_data_info.pause_after_len) {
//      printf("done synt. data block\n");
      pb_type=-1;
      tfr->close_block();
    }
  }
//  fprintf(logfi,"%d\n",cur_level);
}


/*** voice block getsmp ***/

static tb_voice_info_t b_voice_info;
static int voice_cnt;
static unsigned voice_smp;
static int voice_state;        /* 0=playing data 1=playing pause */

static void gs_voice_newblock(void) {
  tfr->get_b_voice_info(&b_voice_info);
  voice_cnt=b_voice_info.smp_len;	/* force gs_voice to load a new sample */
  tfr->open_block();
  voice_state=0;
  
  /* zero-time block? */
  if(b_voice_info.samples==0 && b_voice_info.pause_after_len) {
    pb_type=-1;
    tfr->close_block();
  }
}

static void gs_voice(void) {
  if(voice_state==0) { /* play data */
    if(voice_cnt>=b_voice_info.smp_len) {
      if(tfr->b_voice_getsmps(1,&voice_smp)) { /* end of data */
        voice_state=1;
	voice_cnt=0;
      }
    }
  }
  
  if(voice_state==1) { /* play pause */
    voice_smp=0;
    voice_cnt+=tape_delta_t;
    
    if(voice_cnt>=b_voice_info.pause_after_len) {
      pb_type=-1;
      tfr->close_block();
    }
  }
  
  cur_level=voice_smp;
}

/*** quick load ***/
void zx_tape_ldbytes(void) {
  unsigned req_flag,toload,addr,verify;
  unsigned u;
  u8 flag,b,x,chksum;
  unsigned error;
  int btype;
  tb_data_info_t binfo;
  
  fprintf(logfi,"zx_tape_ldbytes()\n");
  
  fprintf(logfi,"tfr?\n");
  if(!tfr) return;
  fprintf(logfi,"!tape_playing?\n");
  if(tape_playing) return;
  btype=tfr->block_type();
  fprintf(logfi,"btype==BT_DATA?\n");
  if(btype!=BT_DATA) return;
  fprintf(logfi,"!getinfo?\n");
  if(tfr->get_b_data_info(&binfo)) return;
  
  fprintf(logfi,"...\n");
  req_flag=cpus.r_[rA];
  toload=((u16)cpus.r[rD]<<8) | (u16)cpus.r[rE];
  addr=cpus.IX;
  verify=(cpus.F_&fC)?0:1; 
    
  if(tfr->open_block()) return;
  
  error=0;
    
  if(tfr->b_data_getbytes(1,&flag)) error=1;
  
  fprintf(logfi,"req:len %d, flag 0x%02x, addr 0x%04x, verify:%d\n",
          toload,req_flag,addr,verify);
  fprintf(logfi,"block len %u, block flag:0x%02x\n",binfo.data_bytes,flag);
  fprintf(logfi,"z80 F:%02x\n",cpus.F_);
  
  if(flag!=req_flag) error=1;
  
  if(!error) {
    if(verify) fprintf(logfi,"verifying\n");
      else fprintf(logfi,"loading\n");
      
    x=flag;
    for(u=0;u<toload;u++) {
      if(tfr->b_data_getbytes(1,&b)) {
        error=1;
	fprintf(logfi,"out of data\n");
	break;
      }
      if(!verify)
        zx_memset8(addr+u,b);
      x^=b;
    }
  }
  
  if(!error) {
    if(tfr->b_data_getbytes(1,&chksum)) {
      error=1;
      fprintf(logfi,"out of data\n");
    }
  }
  
  if(!error) {
    fprintf(logfi,"stored chksum:$%02x computed:$%02x\n",chksum,x);
    if(chksum!=x) {
      fprintf(logfi,"wrong checksum\n");
      error=1;
    }
  }

  if(error) {
    cpus.F &= ~fC;
  } else {
    cpus.F |= fC;
    fprintf(logfi,"load ok\n");
  }
  
  tfr->close_block();
  
  /* RET */
  fprintf(logfi,"returning\n");
  cpus.PC=zx_memget16(cpus.SP);
  cpus.SP+=2;
}

/*** quick save ***/
void zx_tape_sabytes(void) {
}

/*void zx_tape_sabytes(void) {
  unsigned flag,tosave,addr;
  unsigned x,u,b;
  unsigned error;
  tape_block block;
  
  if(sablock) {
    //printf("sabytes()\n");
    flag=cpus.r_[rA];
    tosave=((u16)cpus.r[rD]<<8) | (u16)cpus.r[rE];
    addr=cpus.IX;
    
    block.type=BT_DATA;
    block.len=tosave+2;
    block.data=malloc(block.len);
    
    block.data[0]=flag;
  
    fprintf(logfi,"wr:len %d, flag 0x%02x, addr 0x%04x\n",
           tosave,flag,addr);
  
    error=0;
      
    fprintf(logfi,"writing\n");
    x=flag;
    for(u=0;u<tosave;u++) {
      b=zx_memget8(addr+u);
      block.data[1+u]=b;
      x^=b;
    }
    block.data[1+tosave]=x;*/ /* write checksum */
    
/*    if(!wtapf) if(w_open("out.tap")<0) return;
    sablock(&block);
    if(flag!=0x00) w_close();*/ /* not a header.. close the file! */
    
/*    cpus.F=error ? (cpus.F&(~fC)) : (cpus.F | fC);
    if(!error) {
      fprintf(logfi,"write ok\n");
    
    
    freeblock(&block);*/
    
    //printf("RET\n");
    /* RET */
/*    cpus.PC=zx_memget16(cpus.SP);
    cpus.SP+=2;
  }
}*/


int zx_tape_selectfile(char *name) {
  char *ext;
  int res;
  
  if(tfr) tfr->close_file();
  pb_type=-1;
  syn_reset();
  
  ext=strrchr(name,'.');
  if(!ext) {
    printf("file has no extension\n");
    return -1;
  }
  
  if(strcmpci(ext,".tap")==0) tfr=&tfr_tap;
    else if(strcmpci(ext,".tzx")==0) tfr=&tfr_tzx;
    else if(strcmpci(ext,".wav")==0) tfr=&tfr_wav;
    else {
      printf("uknown extension '%s'\n",ext);
      return -1;
    }
    
  res=tfr->open_file(name);
    
  if(res<0) {
    printf("error opening tape file\n");
    return -1;
  }
  return 0;
}

int zx_tape_init(int delta_t) {
  tape_playing=0;
  tfr=NULL;
  cur_level=0;
  pb_type=-1;
  tape_delta_t=delta_t;
  return 0;
}

void zx_tape_done(void) {
  if(tfr) tfr->close_file();
}

void zx_tape_getsmp(u8 *smp) {
    
  if(tfr && tape_playing && !tape_paused) {
    if(pb_type<0) { /* no open block */
      /* najdi prvni pouzitelny blok */
      do {
        pb_type=tfr->block_type();
        switch(pb_type) {
          case BT_EOT:     tape_playing=0; pb_type=-1; return;
	  case BT_DATA:    gsmp=gs_data;  gs_data_newblock(); break;
	  case BT_VOICE:   gsmp=gs_voice; gs_voice_newblock(); break;
	  case BT_TONES:   gsmp=gs_tones; gs_tones_newblock(); break;
	  case BT_UNKNOWN: 
	    pb_type=-1;
	    tfr->skip_block();
	    fprintf(logfi,"gesmp: skipping unknown block\n");
	    break;
	}
      } while(pb_type<0);
      
      fprintf(logfi,"getsmp: opening block type %d\n",pb_type);
      fflush(logfi);
    }
    
    /* generuj novy sampl */
    gsmp();
  }
  *smp=cur_level;
}

void zx_tape_play(void) {
  tape_playing=1;
  tape_paused=0;
}

void zx_tape_pause(void) {
  tape_paused=1;
}

void zx_tape_stop(void) {
  tape_playing=0;
}

void zx_tape_rewind(void) {
  if(tfr) tfr->rewind_file();
  pb_type=-1;
}
