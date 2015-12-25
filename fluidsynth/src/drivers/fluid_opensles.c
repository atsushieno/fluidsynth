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

/* fluid_opensles.c
 *
 * Audio driver for OpenSLES.
 *
 */

#include "fluid_synth.h"
#include "fluid_adriver.h"
#include "fluid_settings.h"

#include "config.h"

#include <sys/time.h>
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>

#define NUM_CHANNELS 2

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
  
  fluid_audio_func_t callback;
  void* data;
  int buffer_size;
  fluid_thread_t *thread;
  int cont;
  long next_expected_enqueue_time;
  
  double sample_rate;
} fluid_opensles_audio_driver_t;


fluid_audio_driver_t* new_fluid_opensles_audio_driver(fluid_settings_t* settings,
						   fluid_synth_t* synth);
fluid_audio_driver_t* new_fluid_opensles_audio_driver2(fluid_settings_t* settings,
						    fluid_audio_func_t func, void* data);
int delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p);
void fluid_opensles_audio_driver_settings(fluid_settings_t* settings);
static void fluid_opensles_audio_run(void* d);
static void fluid_opensles_audio_run2(void* d);


void fluid_opensles_audio_driver_settings(fluid_settings_t* settings)
{
  fluid_settings_register_num(settings, "audio.opensles.buffering-sleep-rate", 0.85, 0.0, 1.0,
                              FLUID_HINT_TOGGLED, NULL, NULL);
}


fluid_audio_driver_t*
new_fluid_opensles_audio_driver(fluid_settings_t* settings,
			    fluid_synth_t* synth)
{
  SLresult result;
  fluid_opensles_audio_driver_t* dev;
  double sample_rate;
  int period_size;
  int realtime_prio = 0;
  int err;

  dev = FLUID_NEW(fluid_opensles_audio_driver_t);
  if (dev == NULL) {
    FLUID_LOG(FLUID_ERR, "Out of memory");
    return NULL;
  }

  FLUID_MEMSET(dev, 0, sizeof(fluid_opensles_audio_driver_t));

  fluid_settings_getint(settings, "audio.period-size", &period_size);
  fluid_settings_getnum(settings, "synth.sample-rate", &sample_rate);
  fluid_settings_getint(settings, "audio.realtime-prio", &realtime_prio);

  dev->data = synth;
  dev->buffer_size = period_size;
  dev->sample_rate = sample_rate;
  dev->cont = 1;

  result = slCreateEngine (&(dev->engine), 0, NULL, 0, NULL, NULL);

  if (!dev->engine)
  {
    FLUID_LOG(FLUID_ERR, "Failed to create OpenSLES connection");
    goto error_recovery;
  }
  result = (*dev->engine)->Realize (dev->engine, SL_BOOLEAN_FALSE);
  if (result != 0) goto error_recovery;
  
  SLEngineItf engine_interface;
  result = (*dev->engine)->GetInterface (dev->engine, SL_IID_ENGINE, &engine_interface);
  if (result != 0) goto error_recovery;

  result = (*engine_interface)->CreateOutputMix (engine_interface, &dev->output_mix_object, 0, 0, 0);
  if (result != 0) goto error_recovery;
  
  result = (*dev->output_mix_object)->Realize (dev->output_mix_object, SL_BOOLEAN_FALSE);
  if (result != 0) goto error_recovery;

  SLDataLocator_AndroidSimpleBufferQueue loc_buffer_queue = {
    SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE,
    2 /* number of buffers */
    };
  SLDataFormat_PCM format_pcm = {
    SL_DATAFORMAT_PCM,
    NUM_CHANNELS,
    ((SLuint32) sample_rate) * 1000,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    SL_PCMSAMPLEFORMAT_FIXED_16,
    SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
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
  result = (*engine_interface)->CreateAudioPlayer (engine_interface,
    &(dev->audio_player), &audio_src, &audio_sink, 1, ids1, req1);
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player)->Realize (dev->audio_player,SL_BOOLEAN_FALSE);
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player)->GetInterface (dev->audio_player, 
    SL_IID_PLAY, &(dev->audio_player_interface));
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player)->GetInterface(dev->audio_player,
    SL_IID_ANDROIDSIMPLEBUFFERQUEUE, &(dev->player_buffer_queue_interface));
  if (result != 0) goto error_recovery;

  result = (*dev->audio_player_interface)->SetPlayState(dev->audio_player_interface, SL_PLAYSTATE_PLAYING);
  if (result != 0) goto error_recovery;

  FLUID_LOG(FLUID_INFO, "Using OpenSLES driver");

  /* Create the audio thread */
  dev->thread = new_fluid_thread ("opensles-audio", fluid_opensles_audio_run,
                                  dev, realtime_prio, FALSE);
  if (!dev->thread)
    goto error_recovery;

  return (fluid_audio_driver_t*) dev;

 error_recovery:
  delete_fluid_opensles_audio_driver((fluid_audio_driver_t*) dev);
  return NULL;
}

int delete_fluid_opensles_audio_driver(fluid_audio_driver_t* p)
{
  fluid_opensles_audio_driver_t* dev = (fluid_opensles_audio_driver_t*) p;

  if (dev == NULL) {
    return FLUID_OK;
  }

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
  short *buf;
  int buffer_size;
  int err;
  struct timespec ts;
  long current_time, wait_in_theory;

  buffer_size = dev->buffer_size;

  /* FIXME - Probably shouldn't alloc in run() */
  buf = FLUID_ARRAY(short, buffer_size * NUM_CHANNELS);

  if (buf == NULL)
  {
    FLUID_LOG(FLUID_ERR, "Out of memory.");
    return;
  }

  wait_in_theory = 1000000 * buffer_size / dev->sample_rate;
  int cnt = 0;
  
  while (dev->cont)
  {
    /* compute delta time and update 'next expected enqueue' time */
    clock_gettime(CLOCK_REALTIME, &ts);
    current_time = ts.tv_sec * 1000000 + ts.tv_nsec / 1000;
    long time_delta = dev->next_expected_enqueue_time == 0 ? 0 : dev->next_expected_enqueue_time - current_time;
    if (time_delta == 0)
	  dev->next_expected_enqueue_time += current_time + wait_in_theory;
	else
	  dev->next_expected_enqueue_time += wait_in_theory;
    /* take some sleep only if it's running ahead */
	if (time_delta > 0)
		usleep (time_delta);

    /* it seems that the synth keeps emitting synthesized buffers even if there is no sound. So keep feeding... */
    fluid_synth_write_s16(dev->data, buffer_size, buf, 0, 2, buf, 1, 2);

    SLresult result = (*dev->player_buffer_queue_interface)->Enqueue (
      dev->player_buffer_queue_interface, buf, buffer_size * sizeof (short) * NUM_CHANNELS);
    if (result != 0) {
      err = result;
      /* Do not simply break at just one single insufficient buffer. Go on. */
    }
  }	/* while (dev->cont) */

  FLUID_FREE(buf);
}

