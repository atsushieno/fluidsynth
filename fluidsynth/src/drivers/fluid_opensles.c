/* FluidSynth - A Software Synthesizer
 *
 * Copyright (C) 2003  Peter Hanappe and others.
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Library General Public License
 * as published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Library General Public License for more details.
 *
 * You should have received a copy of the GNU Library General Public
 * License along with this library; if not, write to the Free
 * Software Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA
 * 02110-1301, USA
 */

// ringbuffer implementation from https://audioprograming.wordpress.com/2012/10/29/lock-free-audio-io-with-opensl-es-on-android/

/*
 opensl_io.c:
 Android OpenSL input/output module
 Copyright (c) 2012, Victor Lazzarini
 All rights reserved.
 
Redistribution and use in source and binary forms, with or without
 modification, are permitted provided that the following conditions are met:
 * Redistributions of source code must retain the above copyright
 notice, this list of conditions and the following disclaimer.
 * Redistributions in binary form must reproduce the above copyright
 notice, this list of conditions and the following disclaimer in the
 documentation and/or other materials provided with the distribution.
 * Neither the name of the <organization> nor the
 names of its contributors may be used to endorse or promote products
 derived from this software without specific prior written permission.
 
THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS" AND
 ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE IMPLIED
 WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE
 DISCLAIMED. IN NO EVENT SHALL <COPYRIGHT HOLDER> BE LIABLE FOR ANY
 DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL DAMAGES
 (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR SERVICES;
 LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER CAUSED AND
 ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY, OR TORT
 (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE OF THIS
 SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.
*/

/* fluid_opensles.c
 *
 * Audio driver for OpenSLES.
 *
 */

#include "fluid_synth.h"
#include "fluid_adriver.h"
#include "fluid_settings.h"

#include "config.h"

#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

// <circular_buffer>

#define CONV16BIT 32768

typedef struct _circular_buffer {
 char *buffer;
 int wp;
 int rp;
 int size;
} circular_buffer;
// </circular_buffer>

/** fluid_opensles_audio_driver_t
 *
 * This structure should not be accessed directly. Use audio port
 * functions instead.
 */
typedef struct {
  fluid_audio_driver_t driver;
  SLObjectItf engine;
  SLObjectItf output_mix_object;
  SLObjectItf audio_player;
  SLPlayItf audio_player_interface;
  SLAndroidSimpleBufferQueueItf player_buffer_queue_interface;
  SLresult last_result;
  
  fluid_audio_func_t callback;
  void* data;
  int buffer_size;
  fluid_thread_t *thread;
  int cont;

  // <circular_buffer>
  // buffers
  short *outputBuffer;
  short *playBuffer;
  circular_buffer *outrb;
  circular_buffer *inrb;
  // size of buffers
  int inBufSamples;
  
  double time;  
  int outchannels;
  int sr;
  // </circular_buffer>

} fluid_opensles_audio_driver_t;


fluid_audio_driver_t* new_fluid_opensles_audio_driver(fluid_settings_t* settings,
						   fluid_synth_t* synth);
fluid_audio_driver_t* new_fluid_opensles_audio_driver2(fluid_settings_t* settings,
						    fluid_audio_func_t func, void* data);
int delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p);
void fluid_opensles_audio_driver_settings(fluid_settings_t* settings);
static void fluid_opensles_audio_run(void* d);
static void fluid_opensles_audio_run2(void* d);

// <circular_buffer>
static void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context);
circular_buffer* create_circular_buffer(int bytes);
int checkspace_circular_buffer(circular_buffer *p, int writeCheck);
int read_circular_buffer_bytes(circular_buffer *p, char *out, int bytes);
int write_circular_buffer_bytes(circular_buffer *p, const char *in, int bytes);
void free_circular_buffer (circular_buffer *p);
// </circular_buffer>

void fluid_opensles_audio_driver_settings(fluid_settings_t* settings)
{
/*
  fluid_settings_register_str(settings, "audio.opensles.server", "default", 0, NULL, NULL);
  fluid_settings_register_str(settings, "audio.opensles.device", "default", 0, NULL, NULL);
  fluid_settings_register_str(settings, "audio.opensles.media-role", "music", 0, NULL, NULL);
  fluid_settings_register_int(settings, "audio.opensles.adjust-latency", 1, 0, 1,
                              FLUID_HINT_TOGGLED, NULL, NULL);
*/
}


fluid_audio_driver_t*
new_fluid_opensles_audio_driver(fluid_settings_t* settings,
			    fluid_synth_t* synth)
{
  return new_fluid_opensles_audio_driver2(settings, NULL, synth);
}

fluid_audio_driver_t*
new_fluid_opensles_audio_driver2(fluid_settings_t* settings,
			     fluid_audio_func_t func, void* data)
{
  fluid_opensles_audio_driver_t* dev;
  double sample_rate;
  int period_size, period_bytes, adjust_latency;
  char *server = NULL;
  char *device = NULL;
  char *media_role = NULL;
  int realtime_prio = 0;

  dev = FLUID_NEW(fluid_opensles_audio_driver_t);
  if (dev == NULL) {
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }

  FLUID_MEMSET(dev, 0, sizeof(fluid_opensles_audio_driver_t));

  fluid_settings_getint(settings, "audio.period-size", &period_size);
  fluid_settings_getnum(settings, "synth.sample-rate", &sample_rate);
  fluid_settings_dupstr(settings, "audio.opensles.server", &server);  /* ++ alloc server string */
  fluid_settings_dupstr(settings, "audio.opensles.device", &device);  /* ++ alloc device string */
  fluid_settings_dupstr(settings, "audio.opensles.media-role", &media_role);  /* ++ alloc media-role string */
  fluid_settings_getint(settings, "audio.realtime-prio", &realtime_prio);
  fluid_settings_getint(settings, "audio.opensles.adjust-latency", &adjust_latency);

  if (server && strcmp (server, "default") == 0)
  {
    FLUID_FREE (server);        /* -- free server string */
    server = NULL;
  }

  if (device && strcmp (device, "default") == 0)
  {
    FLUID_FREE (device);        /* -- free device string */
    device = NULL;
  }

  dev->data = data;
  dev->callback = func;
  dev->cont = 1;
  dev->buffer_size = period_size;

  dev->outchannels = 2;

  period_bytes = period_size * sizeof (float) * 2;
  ////bufattr.maxlength = adjust_latency ? -1 : period_bytes;
  ////bufattr.tlength = period_bytes;
  ////bufattr.minreq = -1;
  ////bufattr.prebuf = -1;    /* Just initialize to same value as tlength */
  ////bufattr.fragsize = -1;  /* Not used */

  dev->last_result = slCreateEngine (&(dev->engine), 0, NULL, 0, NULL, NULL);

  if (!dev->engine)
  {
    FLUID_LOG(FLUID_ERR, "Failed to create OpenSLES connection");
    goto error_recovery;
  }
  dev->last_result = (*dev->engine)->Realize (dev->engine, SL_BOOLEAN_FALSE);
  if (dev->last_result != 0) goto error_recovery;
  
  SLEngineItf engine_interface;
  dev->last_result = (*dev->engine)->GetInterface (dev->engine, SL_IID_ENGINE, &engine_interface);
  if (dev->last_result != 0) goto error_recovery;

  dev->last_result = (*engine_interface)->CreateOutputMix (engine_interface, &dev->output_mix_object, 0, 0, 0);
  if (dev->last_result != 0) goto error_recovery;
  
  dev->last_result = (*dev->output_mix_object)->Realize (dev->output_mix_object, SL_BOOLEAN_FALSE);
  if (dev->last_result != 0) goto error_recovery;

  SLDataLocator_AndroidSimpleBufferQueue loc_buffer_queue = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
    2
    };
    /* TODO: verify sampling rate. Since it accepts only some values as valid ones, I ignored sample_rate and specified this directly. */
  SLDataFormat_PCM format_pcm = {
    SL_DATAFORMAT_PCM,
    2, /* numChannels */
    SL_SAMPLINGRATE_44_1,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    0, /* channelMask */
    SL_BYTEORDER_LITTLEENDIAN
    };
  SLDataSource audio_src = {
    &loc_buffer_queue,
    &format_pcm
    };

  SLDataLocator_OutputMix loc_outmix = {
    SL_DATALOCATOR_OUTPUTMIX,
    dev->output_mix_object
    };
  SLDataSink audio_sink = {&loc_outmix, NULL};

  const SLInterfaceID ids1[] = {SL_IID_ANDROIDSIMPLEBUFFERQUEUE};
  const SLboolean req1[] = {SL_BOOLEAN_TRUE};
  dev->last_result = (*engine_interface)->CreateAudioPlayer (engine_interface,
    &(dev->audio_player), &audio_src, &audio_sink, 1, ids1, req1);
  if (dev->last_result != 0) goto error_recovery;

  dev->last_result = (*dev->audio_player)->Realize (dev->audio_player,SL_BOOLEAN_FALSE);
  if (dev->last_result != 0) goto error_recovery;

  dev->last_result = (*dev->audio_player)->GetInterface (dev->audio_player, 
    SL_IID_PLAY, &(dev->audio_player_interface));
  if (dev->last_result != 0) goto error_recovery;

  dev->last_result = (*dev->audio_player)->GetInterface(dev->audio_player,
    SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &(dev->player_buffer_queue_interface));
  if (dev->last_result != 0) goto error_recovery;

  dev->last_result = (*dev->player_buffer_queue_interface)->RegisterCallback(dev->player_buffer_queue_interface,
    bqPlayerCallback, dev);
  if (dev->last_result != 0) goto error_recovery;

  if ((dev->outputBuffer = (short *) calloc(dev->buffer_size, sizeof(short))) == NULL)
    goto error_recovery;
  if ((dev->playBuffer = (short *) calloc(dev->buffer_size, sizeof(float))) == NULL)
    goto error_recovery;
  if ((dev->outrb = create_circular_buffer(dev->buffer_size * sizeof(short) * 4)) == NULL)
    goto error_recovery;
   
  /* Create the audio thread */
  dev->thread = new_fluid_thread ("opensles-audio", func ? fluid_opensles_audio_run2 : fluid_opensles_audio_run,
                                  dev, realtime_prio, FALSE);
  if (!dev->thread)
    goto error_recovery;

  FLUID_LOG(FLUID_INFO, "Using OpenSLES driver__");

  dev->last_result = (*dev->audio_player_interface)->SetPlayState(dev->audio_player_interface, SL_PLAYSTATE_PLAYING);
  if (dev->last_result != 0) goto error_recovery;
  
  dev->last_result = (*dev->player_buffer_queue_interface)->Enqueue (
      dev->player_buffer_queue_interface, dev->playBuffer, dev->buffer_size * sizeof (float) * 2);
  if (dev->last_result != 0) goto error_recovery;

  if (server) FLUID_FREE (server);      /* -- free server string */
  if (device) FLUID_FREE (device);      /* -- free device string */

  return (fluid_audio_driver_t*) dev;

 error_recovery:
  if (server) FLUID_FREE (server);      /* -- free server string */
  if (device) FLUID_FREE (device);      /* -- free device string */
  delete_fluid_opensles_audio_driver((fluid_audio_driver_t*) dev);
  return NULL;
}

int delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p)
{
  fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) p;

  if (dev == NULL) {
    return FLUID_OK;
  }

  //FLUID_LOG(FLUID_INFO, "terminating audio run...");
  
  dev->cont = 0;

  if (dev->thread)
    fluid_thread_join (dev->thread);

  if (dev->audio_player)
    (*dev->audio_player)->Destroy (dev->audio_player);
  if (dev->output_mix_object)
    (*dev->output_mix_object)->Destroy (dev->output_mix_object);
  if (dev->engine)
    (*dev->engine)->Destroy (dev->engine);

  FLUID_FREE(dev);

  return FLUID_OK;
}

/* Thread without audio callback, more efficient */
static void
fluid_opensles_audio_run(void* d)
{
  fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) d;
  float *buf;
  int buffer_size, written_size;

  //FLUID_LOG(FLUID_INFO, "start audio run. %d", dev->buffer_size);
  
  /* FIXME - Probably shouldn't alloc in run() */
  buf = FLUID_ARRAY(float, buffer_size * 2);

  if (buf == NULL)
  {
    FLUID_LOG(FLUID_ERR, "Out of memory.");
    return;
  }
  
  while (dev->cont)
  {
    fluid_synth_write_float(dev->data, buffer_size, buf, 0, 2, buf, 1, 2);
    written_size = android_AudioOut (dev, buf, dev->buffer_size * sizeof (float) * 2);
  }	/* while (dev->cont) */

  FLUID_FREE(buf);

  FLUID_LOG(FLUID_INFO, "end audio run.");  
}

static void
fluid_opensles_audio_run2(void* d)
{
  fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) d;
  fluid_synth_t *synth = (fluid_synth_t *)(dev->data);
  float *left, *right, *buf;
  float* handle[2];
  int buffer_size;
  int i;

  FLUID_LOG(FLUID_INFO, "start audio run2. %d", dev->buffer_size);
  
  /* FIXME - Probably shouldn't alloc in run() */
  left = FLUID_ARRAY(float, dev->buffer_size);
  right = FLUID_ARRAY(float, dev->buffer_size);
  buf = FLUID_ARRAY(float, dev->buffer_size * 2);

  if (left == NULL || right == NULL || buf == NULL)
  {
    FLUID_LOG(FLUID_ERR, "Out of memory.");
    return;
  }

  if (dev->last_result != 0)
  {
    FLUID_LOG(FLUID_ERR, "Invalid audio state, skip audio run2.");
    return;
  }

  handle[0] = left;
  handle[1] = right;

  while (dev->cont)
  {
    (*dev->callback)(synth, dev->buffer_size, 0, NULL, 2, handle);

    /* Interleave the floating point data */
    for (i = 0; i < buffer_size; i++)
    {
      buf[i * 2] = left[i];
      buf[i * 2 + 1] = right[i];
    }

    dev->last_result = (*dev->player_buffer_queue_interface)->Enqueue (
      dev->player_buffer_queue_interface, buf, dev->buffer_size * sizeof (float) * 2);
    if (dev->last_result != 0) {
      FLUID_LOG(FLUID_ERR, "Error writing to OpenSLES connection.");
      break;
    }
  }	/* while (dev->cont) */

  FLUID_FREE(left);
  FLUID_FREE(right);
  FLUID_FREE(buf);

  FLUID_LOG(FLUID_INFO, "end audio run2.");  
}

// <circular_buffer>
 
// this callback handler is called every time a buffer finishes playing
void bqPlayerCallback(SLAndroidSimpleBufferQueueItf bq, void *context)
{
 fluid_opensles_audio_driver_t *p = (fluid_opensles_audio_driver_t *) context;
 int bytes = p->buffer_size;
 read_circular_buffer_bytes(p->outrb, (char *) p->playBuffer,bytes);
 (*p->player_buffer_queue_interface)->Enqueue(p->player_buffer_queue_interface,p->playBuffer,bytes);
}
 
// puts a buffer of size samples to the device
int android_AudioOut(fluid_opensles_audio_driver_t *p, float *buffer,int size){
 
  short *outBuffer, *inBuffer;
 int i, bytes = size*sizeof(short);
 if(p == NULL || p->buffer_size == 0) return 0;
 for(i=0; i < size; i++){
 p->outputBuffer[i] = (short) (buffer[i]*CONV16BIT);
 }
 bytes = write_circular_buffer_bytes(p->outrb, (char *) p->outputBuffer,bytes);
 p->time += (double) size/(p->sr*p->outchannels);
 return bytes/sizeof(short);
}
 
circular_buffer* create_circular_buffer(int bytes){
 circular_buffer *p;
 if ((p = calloc(1, sizeof(circular_buffer))) == NULL) {
 return NULL;
 }
 p->size = bytes;
 p->wp = p->rp = 0;
 
 if ((p->buffer = calloc(bytes, sizeof(char))) == NULL) {
 free (p);
 return NULL;
 }
 return p;
}
 
int checkspace_circular_buffer(circular_buffer *p, int writeCheck){
 int wp = p->wp, rp = p->rp, size = p->size;
 if(writeCheck){
 if (wp > rp) return rp - wp + size - 1;
 else if (wp < rp) return rp - wp - 1;
 else return size - 1;
 }
 else {
 if (wp > rp) return wp - rp;
 else if (wp < rp) return wp - rp + size;
 else return 0;
 }
}
 
int read_circular_buffer_bytes(circular_buffer *p, char *out, int bytes){
 int remaining;
 int bytesread, size = p->size;
 int i=0, rp = p->rp;
 char *buffer = p->buffer;
 if ((remaining = checkspace_circular_buffer(p, 0)) == 0) {
 return 0;
 }
 bytesread = bytes > remaining ? remaining : bytes;
 for(i=0; i < bytesread; i++){
 out[i] = buffer[rp++];
 if(rp == size) rp = 0;
 }
 p->rp = rp;
 return bytesread;
}
 
int write_circular_buffer_bytes(circular_buffer *p, const char *in, int bytes){
 int remaining;
 int byteswrite, size = p->size;
 int i=0, wp = p->wp;
 char *buffer = p->buffer;
 if ((remaining = checkspace_circular_buffer(p, 1)) == 0) {
 return 0;
 }
 byteswrite = bytes > remaining ? remaining : bytes;
 for(i=0; i < byteswrite; i++){
 buffer = in[i];
 if(wp == size) wp = 0;
 }
 p->wp = wp;
 return byteswrite;
}
 
void
free_circular_buffer (circular_buffer *p){
 if(p == NULL) return;
 free(p->buffer);
 free(p);
}

// </circular_buffer>
