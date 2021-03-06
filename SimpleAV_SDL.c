/* Copyright (C) 2011 by Chenguang Wang(wecing)
 *
 * This program is free software. It comes without any warranty, to
 * the extent permitted by applicable law. You can redistribute it
 * and/or modify it under the terms of the Do What The Fuck You Want
 * To Public License, Version 2, as published by Sam Hocevar. See
 * http://sam.zoy.org/wtfpl/COPYING for more details.
 */

#ifdef __cplusplus
extern "C" {
#endif

#include <stdio.h>
#include <stdlib.h>

#include <libswscale/swscale.h>

#include <SimpleAV.h>

#include "SimpleAV_SDL.h"

// private helper functions.

// convert the video frame in sasdl_ctx->frame_next onto sasdl_ctx->frame_cur.
static void _SASDL_convert_frame_next_to_cur(SASDLContext *);

// fill sasdl_ctx->frame_cur with black.
// currently only used in SASDL_draw() under "stopped" mode.
static void _SASDL_fill_frame_cur_black(SASDLContext *);

// called by SASDL_seek() and SASDL_seek_accurate().
static int _SASDL_seek_internal(SASDLContext *sasdl_ctx,
                                double seek_dst, int accurate);

int SASDL_init(void)
{
     // FIXME: "silent" mode?
     SA_init();
     return 0;
}

SASDLContext *SASDL_open(char *filename)
{
     SAContext *sa_ctx = SA_open(filename);
     if(sa_ctx == NULL)
     {
          fprintf(stderr, "SA_open failed.\n");
          return NULL;
     }

     SASDLContext *sasdl_ctx = malloc(sizeof(SASDLContext));
     if(sasdl_ctx == NULL)
     {
          SA_close(sa_ctx);
          fprintf(stderr, "malloc for sasdl_ctx failed!\n");
          return NULL;
     } else
     {
          memset(sasdl_ctx, 0, sizeof(SASDLContext));
          
          sasdl_ctx->status = SASDL_is_stopped;
          sasdl_ctx->video_restart_at = 0.0f;
          // sasdl_ctx->start_time = 0.0f;
          
          sasdl_ctx->sa_ctx = sa_ctx;

          sasdl_ctx->audio_mix_volume = SDL_MIX_MAXVOLUME;
     }
     
     int width = SASDL_get_video_width(sasdl_ctx);
     int height = SASDL_get_video_height(sasdl_ctx);

     // FIXME: is SWS_FAST_BILINEAR the fastest?
     //        we don't change the size here. so speed is everything.
     struct SwsContext *swsctx = sws_getContext(width, height, sa_ctx->v_codec_ctx->pix_fmt,
                                                width, height, PIX_FMT_RGB32, SWS_FAST_BILINEAR,
                                                NULL, NULL, NULL);
     if(swsctx == NULL)
     {
          SASDL_close(sasdl_ctx);
          fprintf(stderr, "sws_getContext failed!\n");
          return NULL;
     } else
          sasdl_ctx->swsctx = swsctx;

     sasdl_ctx->ap_lock = SDL_CreateMutex();
     sasdl_ctx->frame_cur = SDL_CreateRGBSurface(SDL_SWSURFACE, width, height, 32,
                                                 0, 0, 0, 0);
     if(sasdl_ctx->frame_cur == NULL) {
          SASDL_close(sasdl_ctx);
          fprintf(stderr, "failed to create SDL RGB surface.\n");
          return NULL;
     }

     _SASDL_fill_frame_cur_black(sasdl_ctx);

     return sasdl_ctx;
}

int SASDL_close(SASDLContext *sasdl_ctx)
{
     if(sasdl_ctx == NULL)
          return -1;

     if(sasdl_ctx->ap_lock != NULL)
     {
          SDL_mutexP(sasdl_ctx->ap_lock);
          if(sasdl_ctx->ap != NULL)
               SA_free_ap(sasdl_ctx->ap);
          SDL_DestroyMutex(sasdl_ctx->ap_lock);
     }

     if(sasdl_ctx->swsctx != NULL)
          sws_freeContext(sasdl_ctx->swsctx);

     if(sasdl_ctx->frame_cur != NULL)
          SDL_FreeSurface(sasdl_ctx->frame_cur);
     
     SA_close(sasdl_ctx->sa_ctx);
     
     free(sasdl_ctx);
     return 0;
}

void SASDL_set_audio_mix(SASDLContext *sasdl_ctx, int value)
{
     sasdl_ctx->audio_decode_mix = (value ? 1 : 0);
}
     
void SASDL_set_mix_volume(SASDLContext *sasdl_ctx, int value) {
     if(sasdl_ctx->audio_decode_mix) {
          if(value < 0) {
               value = 0;
          } else if(value > SDL_MIX_MAXVOLUME) {
               value = SDL_MIX_MAXVOLUME;
          }
          sasdl_ctx->audio_mix_volume = value;
     }
}

void SASDL_play(SASDLContext *sasdl_ctx)
{
     if(sasdl_ctx->status == SASDL_is_playing)
          return;
     sasdl_ctx->status = SASDL_is_playing;
     sasdl_ctx->start_time = av_gettime() - (int64_t)((sasdl_ctx->video_restart_at) * 1000000.0f);
}


void SASDL_pause(SASDLContext *sasdl_ctx)
{
     if(sasdl_ctx->status != SASDL_is_playing)
          return;
     sasdl_ctx->video_restart_at = SASDL_get_video_clock(sasdl_ctx);
     sasdl_ctx->status = SASDL_is_paused;
}

int SASDL_stop(SASDLContext *sasdl_ctx)
{
     if(sasdl_ctx->status == SASDL_is_stopped)
          return 0;
     sasdl_ctx->status = SASDL_is_stopped;
     sasdl_ctx->video_restart_at = 0.0f;
     
     // SASDL_seek will fill frame_cur with black for us.
     int ret = SASDL_seek(sasdl_ctx, 0.0f);

     // SASDL_seek() will set the video status to paused if it is stopped;
     // so we need to set it back to stopped here.
     sasdl_ctx->status = SASDL_is_stopped;
     
     return ret;
}

int SASDL_seek(SASDLContext *sasdl_ctx, double seek_dst) {
     return _SASDL_seek_internal(sasdl_ctx, seek_dst, 0);
}

int SASDL_seek_accurate(SASDLContext *sasdl_ctx, double seek_dst) {
     return _SASDL_seek_internal(sasdl_ctx, seek_dst, 1);
}

// currently, SASDL_seek() will return -1 on both EOF and error.
// so the user should directly stop the video loop when receiving -1.
static int _SASDL_seek_internal(SASDLContext *sasdl_ctx,
                                double seek_dst, int accurate)
{
     if(seek_dst >= SASDL_get_video_duration(sasdl_ctx))
          return SASDL_stop(sasdl_ctx);
     
     if(seek_dst < 0.0f)
          seek_dst = 0.0f;

     int ret = SA_seek(sasdl_ctx->sa_ctx, seek_dst);
     if(ret < 0)
          return ret;

     SDL_mutexP(sasdl_ctx->ap_lock);
     SAAudioPacket *ap = sasdl_ctx->ap;
     if(ap != NULL)
     {
          SA_free_ap(ap);
          sasdl_ctx->ap = NULL;
          sasdl_ctx->audio_buf_index = 0;
     }

     SAVideoPacket *vp = SA_get_vp(sasdl_ctx->sa_ctx);
     if(vp == NULL) {
          sasdl_ctx->video_eof = TRUE;
          sasdl_ctx->frame_next = NULL;

          // *FIXME*: is releasing the lock here proper?
          //          (this line is a hack for saya)
          SDL_mutexV(sasdl_ctx->ap_lock);
          
          return -1;
     } else {
          sasdl_ctx->frame_next = vp->frame_ptr;
          sasdl_ctx->frame_next_pts = vp->pts;
          SA_free_vp(vp);

          if(seek_dst != 0.0f)
               _SASDL_convert_frame_next_to_cur(sasdl_ctx);
          else
               _SASDL_fill_frame_cur_black(sasdl_ctx);
     }

     if (accurate) {
          while (sasdl_ctx->frame_next_pts < seek_dst) {
              vp = SA_get_vp(sasdl_ctx->sa_ctx);
              if (vp == NULL) {
                  sasdl_ctx->video_eof = TRUE;
                  sasdl_ctx->frame_next = NULL;

                  // FIXME: if the mutexV() out of the if-accurate statement
                  //        is modified, change here as well.
                  SDL_mutexV(sasdl_ctx->ap_lock);

                  return -1;
              } else {
                  if (vp->pts > seek_dst) {
                      _SASDL_convert_frame_next_to_cur(sasdl_ctx);
                  }
                  sasdl_ctx->frame_next = vp->frame_ptr;
                  sasdl_ctx->frame_next_pts = vp->pts;
                  SA_free_vp(vp);
              }
          }
     } else {
          // the real position we have reached.
          seek_dst = sasdl_ctx->frame_next_pts;
     }

     ap = SA_get_ap(sasdl_ctx->sa_ctx);
     if(ap == NULL) {
          sasdl_ctx->audio_eof = TRUE;
     } else {
          while(ap->pts < seek_dst) {
               SA_free_ap(ap);
               ap = SA_get_ap(sasdl_ctx->sa_ctx);
               if(ap == NULL) {
                    sasdl_ctx->audio_eof = TRUE;
                    break;
               }
          }
          sasdl_ctx->ap = ap;
     }
     SDL_mutexV(sasdl_ctx->ap_lock);

     if(sasdl_ctx->status == SASDL_is_playing)
          sasdl_ctx->start_time = av_gettime() - (int64_t)(seek_dst * 1000000.0f);
     else {
          sasdl_ctx->video_restart_at = seek_dst;
          if(sasdl_ctx->status == SASDL_is_stopped)
               sasdl_ctx->status = SASDL_is_paused;
     }

     if(seek_dst < SASDL_get_video_duration(sasdl_ctx)) {
          sasdl_ctx->video_eof = sasdl_ctx->audio_eof = FALSE;
     } else {
          sasdl_ctx->video_eof = sasdl_ctx->audio_eof = TRUE;
          SASDL_stop(sasdl_ctx);
     }

     return ret;
}

/*
 * SASDL_draw() always draw the correct frame,
 * even when the user is not using SASDL_delay().
 *
 * "correct frame" means, we will always have:
 *     (pts of frame_cur <=) video clock < frame_next_pts
 * under the "playing" mode.
 */
void SASDL_draw(SASDLContext *sasdl_ctx, SDL_Surface *surface)
{
     if(sasdl_ctx->status != SASDL_is_playing ||
        sasdl_ctx->video_eof) {
          SDL_BlitSurface(sasdl_ctx->frame_cur, NULL, surface, NULL);
          return;
     }

     // From now on, we are sure of these facts:
     //   1. the video is under the "playing" mode.
     //   2. we have not reach EOF of the video yet.
     
     SAVideoPacket *vp;
     while(sasdl_ctx->frame_next == NULL ||
           sasdl_ctx->frame_next_pts <= SASDL_get_video_clock(sasdl_ctx)) {
          vp = SA_get_vp(sasdl_ctx->sa_ctx);
          if(vp == NULL) {
               sasdl_ctx->video_eof = TRUE;
               sasdl_ctx->frame_next = NULL;
               break;
          } else {
               sasdl_ctx->frame_next = vp->frame_ptr;
               sasdl_ctx->frame_next_pts = vp->pts;
               SA_free_vp(vp);

               _SASDL_convert_frame_next_to_cur(sasdl_ctx);
          }
     }

     SDL_BlitSurface(sasdl_ctx->frame_cur, NULL, surface, NULL);
     return;
}

/*
 * wait for the next frame.
 * return instantly when not under "playing" mode or encountered video EOF,
 * or we already have next_frame_pts <= video clock.
 */
void SASDL_wait_for_next_frame(SASDLContext *sasdl_ctx)
{
     if(sasdl_ctx->status != SASDL_is_playing ||
        sasdl_ctx->video_eof) {
          return;
     }
     
     double w_time = SASDL_get_video_clock(sasdl_ctx) - sasdl_ctx->frame_next_pts;
     if(w_time > 0.0f) {
          // FIXME: do we need a 2nd iteration?
          SDL_Delay(w_time * 1000);
     }
     
     return;
}

// attention please: the first parameter is sasdl_ctx,
// but NOT sa_ctx!!!!!!!!
//
// output silence if encountered audio EOF.
void SASDL_audio_decode(void *data, uint8_t *stream, int len)
{
     SASDLContext *sasdl_ctx = data;
     SAContext *sa_ctx = sasdl_ctx->sa_ctx;

     if(sasdl_ctx->audio_eof || sasdl_ctx->status != SASDL_is_playing) {
          if(sasdl_ctx->audio_decode_mix == FALSE)
               memset(stream, 0, len);
          return;
     }

     SDL_mutexP(sasdl_ctx->ap_lock);
     
     SAAudioPacket *ap = sasdl_ctx->ap;
     unsigned int audio_buf_index = sasdl_ctx->audio_buf_index;
     unsigned int size_to_copy = 0;
     double size_per_sec = 2 * sa_ctx->a_codec_ctx->channels *
                           sa_ctx->a_codec_ctx->sample_rate;

     while(len > 0)
     {
          if(ap == NULL)
               sasdl_ctx->ap = ap = SA_get_ap(sa_ctx);

          if(ap == NULL)
          {
               sasdl_ctx->audio_eof = TRUE;
               if(sasdl_ctx->audio_decode_mix == FALSE)
                    memset(stream, 0, len);
               sasdl_ctx->ap = NULL;
               sasdl_ctx->audio_buf_index = audio_buf_index;
               SDL_mutexV(sasdl_ctx->ap_lock);
               return; // FIXME: *MAYBE* eof encountered. what if... ?
          }

          double delay = ap->pts - SASDL_get_video_clock(sasdl_ctx);
          if(-SASDL_AUDIO_ADJUST_THRESHOLD <= delay &&
             delay <= SASDL_AUDIO_ADJUST_THRESHOLD)
               delay = 0.0f;
          int delay_size = delay * size_per_sec;
          if(delay_size > 0) // 'wait' for the external clock
          {
               int silent_size = len < delay_size ? len : delay_size;
               if(sasdl_ctx->audio_decode_mix == FALSE)
                    memset(stream, 0, silent_size);
               len -= silent_size;
               stream += silent_size;
               continue;
          } else if(delay_size < 0) // shrink the buffer
          {
               audio_buf_index -= delay_size;
               
               if(audio_buf_index >= ap->len)
               {
                    SA_free_ap(ap);
                    ap = NULL;
                    audio_buf_index = 0;
                    continue;
               }
          }
          
          size_to_copy = len < (ap->len - audio_buf_index) ?
               len : (ap->len - audio_buf_index);
          if(sasdl_ctx->audio_decode_mix == FALSE) {
               memcpy(stream, ap->abuffer + audio_buf_index, size_to_copy);
          } else {
               SDL_MixAudio(stream, ap->abuffer + audio_buf_index,
                            size_to_copy, sasdl_ctx->audio_mix_volume);
          }

          len -= size_to_copy;
          stream += size_to_copy;
          audio_buf_index += size_to_copy;

          if(audio_buf_index >= ap->len)
          {
               SA_free_ap(ap);
               ap = NULL;
               audio_buf_index = 0;
          }
     }

     sasdl_ctx->ap = ap;
     sasdl_ctx->audio_buf_index = audio_buf_index;
     SDL_mutexV(sasdl_ctx->ap_lock);
}


/*
 * other functions - welcome to the easy part!
 */

int SASDL_get_video_width(SASDLContext *sasdl_ctx)
{
     return SA_get_width(sasdl_ctx->sa_ctx);
}

int SASDL_get_video_height(SASDLContext *sasdl_ctx)
{
     return SA_get_height(sasdl_ctx->sa_ctx);
}

double SASDL_get_video_duration(SASDLContext *sasdl_ctx)
{
     return SA_get_duration(sasdl_ctx->sa_ctx);
}

void SASDL_dump_info(SASDLContext *sasdl_ctx)
{
     SA_dump_info(sasdl_ctx->sa_ctx);
}

double SASDL_get_video_clock(SASDLContext *sasdl_ctx)
{
     if(sasdl_ctx->status == SASDL_is_playing)
          return (double)(av_gettime() - sasdl_ctx->start_time) / (double)1000000.0f;
     if(sasdl_ctx->status == SASDL_is_paused)
          return sasdl_ctx->video_restart_at;
     return 0.0f;
}

enum SASDLVideoStatus SASDL_get_video_status(SASDLContext *sasdl_ctx)
{
     return sasdl_ctx->status;
}

int SASDL_video_is_playing(SASDLContext *sasdl_ctx)
{
     return sasdl_ctx->status == SASDL_is_playing ? 1 : 0;
}

int SASDL_video_is_paused(SASDLContext *sasdl_ctx)
{
     return sasdl_ctx->status == SASDL_is_paused ? 1 : 0;
}

int SASDL_video_is_stopped(SASDLContext *sasdl_ctx)
{
     return sasdl_ctx->status == SASDL_is_stopped ? 1 : 0;
}

int SASDL_eof(SASDLContext *sasdl_ctx)
{
     return sasdl_ctx->video_eof && sasdl_ctx->audio_eof;
}

////
//// private helper methods.
////

static void _SASDL_convert_frame_next_to_cur(SASDLContext *sasdl_ctx)
{
     AVFrame *frame = sasdl_ctx->frame_next;
     SDL_Surface *surface = sasdl_ctx->frame_cur;
     int h = SASDL_get_video_height(sasdl_ctx);
     
     SDL_LockSurface(surface);
     AVPicture pict;
     pict.data[0] = surface->pixels; 
     pict.linesize[0] = surface->pitch;
     sws_scale(sasdl_ctx->swsctx, (const uint8_t * const *)(frame->data),
               frame->linesize, 0, h, pict.data, pict.linesize);
     SDL_UnlockSurface(surface);
}

static void _SASDL_fill_frame_cur_black(SASDLContext *sasdl_ctx)
{
     SDL_Rect full_screen = {
          .x = 0, .y = 0,
          .w = SASDL_get_video_width(sasdl_ctx), .h = SASDL_get_video_height(sasdl_ctx)
     };
     
     SDL_FillRect(sasdl_ctx->frame_cur, &full_screen, 0x000000);
}

#ifdef __cplusplus
}
#endif
