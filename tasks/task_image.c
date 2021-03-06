/*  RetroArch - A frontend for libretro.
 *  Copyright (C) 2011-2017 - Daniel De Matteis
 *
 *  RetroArch is free software: you can redistribute it and/or modify it under the terms
 *  of the GNU General Public License as published by the Free Software Found-
 *  ation, either version 3 of the License, or (at your option) any later version.
 *
 *  RetroArch is distributed in the hope that it will be useful, but WITHOUT ANY WARRANTY;
 *  without even the implied warranty of MERCHANTABILITY or FITNESS FOR A PARTICULAR
 *  PURPOSE.  See the GNU General Public License for more details.
 *
 *  You should have received a copy of the GNU General Public License along with RetroArch.
 *  If not, see <http://www.gnu.org/licenses/>.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>

#include <file/nbio.h>
#include <formats/image.h>
#include <compat/strl.h>
#include <retro_miscellaneous.h>

#include "../gfx/video_driver.h"
#include "../file_path_special.h"
#include "../verbosity.h"

#include "tasks_internal.h"

enum image_status_enum
{
   IMAGE_STATUS_POLL = 0,
   IMAGE_STATUS_TRANSFER,
   IMAGE_STATUS_TRANSFER_PARSE,
   IMAGE_STATUS_PROCESS_TRANSFER,
   IMAGE_STATUS_PROCESS_TRANSFER_PARSE,
   IMAGE_STATUS_TRANSFER_PARSE_FREE
};

struct nbio_image_handle
{
   enum image_type_enum type;
   struct texture_image ti;
   bool is_blocking;
   bool is_blocking_on_processing;
   bool is_finished;
   transfer_cb_t  cb;
   void *handle;
   size_t size;
   unsigned processing_pos_increment;
   unsigned pos_increment;
   int processing_final_state;
   enum image_status_enum status;
};

static int cb_image_menu_upload_generic(void *data, size_t len)
{
   unsigned r_shift, g_shift, b_shift, a_shift;
   nbio_handle_t             *nbio = (nbio_handle_t*)data;
   struct nbio_image_handle *image = (struct nbio_image_handle*)nbio->data;

   if (!image)
      return -1;

   switch (image->processing_final_state)
   {
      case IMAGE_PROCESS_ERROR:
      case IMAGE_PROCESS_ERROR_END:
         return -1;
      default:
         break;
   }

   image_texture_set_color_shifts(&r_shift, &g_shift, &b_shift,
         &a_shift, &image->ti);

   image_texture_color_convert(r_shift, g_shift, b_shift,
         a_shift, &image->ti);

   image->is_blocking_on_processing         = false;
   image->is_blocking                       = true;
   image->is_finished                       = true;
   nbio->is_finished                        = true;

   return 0;
}

static int task_image_process(
      struct nbio_image_handle *image,
      unsigned *width,
      unsigned *height)
{
   int retval = image_transfer_process(
         image->handle,
         image->type,
         &image->ti.pixels, image->size, width, height);

   if (retval == IMAGE_PROCESS_ERROR)
      return IMAGE_PROCESS_ERROR;

   image->ti.width  = *width;
   image->ti.height = *height;

   return retval;
}

static int task_image_menu_generic(struct nbio_image_handle *image)
{
   unsigned width                  = 0;
   unsigned height                 = 0;
   int retval                      = task_image_process(image, &width, &height);

   switch (retval)
   {
      case IMAGE_PROCESS_ERROR:
      case IMAGE_PROCESS_ERROR_END:
         return -1;
      default:
         break;
   }

   image->is_blocking_on_processing         = (retval != IMAGE_PROCESS_END);
   image->is_finished                       = (retval == IMAGE_PROCESS_END);

   return 0;
}

static int cb_image_menu_thumbnail(void *data, size_t len)
{
   nbio_handle_t        *nbio = (nbio_handle_t*)data; 
   struct nbio_image_handle *image = (struct nbio_image_handle*)nbio->data;

   if (!image || task_image_menu_generic(image) != 0)
      return -1;

   image->cb = &cb_image_menu_upload_generic;

   return 0;
}

static int task_image_iterate_process_transfer(struct nbio_image_handle *image)
{
   unsigned i;
   int retval                      = 0;
   unsigned width                  = 0;
   unsigned height                 = 0;

   if (!image)
      return -1;

   for (i = 0; i < image->processing_pos_increment; i++)
   {
      retval = task_image_process(image,
               &width, &height);
      if (retval != IMAGE_PROCESS_NEXT)
         break;
   }

   if (retval == IMAGE_PROCESS_NEXT)
      return 0;

   image->processing_final_state = retval;
   return -1;
}

static int task_image_iterate_transfer(struct nbio_image_handle *image)
{
   unsigned i;

   if (!image)
      goto error;

   if (image->is_finished)
      return 0;

   for (i = 0; i < image->pos_increment; i++)
   {
      if (!image_transfer_iterate(image->handle, image->type))
         goto error;
   }

   return 0;

error:
   return -1;
}

static void task_image_load_free_internal(struct nbio_image_handle *image)
{
   image_transfer_free(image->handle, image->type);

   image->handle                 = NULL;
   image->cb                     = NULL;
}

static void task_image_load_free(retro_task_t *task)
{
   nbio_handle_t       *nbio  = task ? (nbio_handle_t*)task->state : NULL;

   if (nbio)
   {
      struct nbio_image_handle *image = (struct nbio_image_handle*)nbio->data;
      
      if (image)
         task_image_load_free_internal(image);
      if (nbio->data)
         free(nbio->data);
      nbio_free(nbio->handle);
      nbio->data        = NULL;
      nbio->handle      = NULL;
      free(nbio);
   }
}

static int cb_nbio_generic(nbio_handle_t *nbio, size_t *len)
{
   struct nbio_image_handle *image = (struct nbio_image_handle*)nbio->data;
   void *ptr                       = nbio_get_ptr(nbio->handle, len);

   if (!ptr || !image || !image->handle)
      goto error;

   image_transfer_set_buffer_ptr(image->handle, image->type, ptr);

   image->size                     = *len;
   image->pos_increment            = (*len / 2) ? ((unsigned)(*len / 2)) : 1;
   image->processing_pos_increment = (*len / 4) ?
       ((unsigned)(*len / 4)) : 1;

   if (!image_transfer_start(image->handle, image->type))
      goto error;

   image->is_blocking   = false;
   image->is_finished   = false;
   nbio->is_finished    = true;

   return 0;

error:
   if (image)
      task_image_load_free_internal(image);
   if (nbio->data)
      free(nbio->data);
   nbio->data = NULL;
   return -1;
}

static int cb_nbio_image_menu_thumbnail(void *data, size_t len)
{
   void *handle                    = NULL;
   nbio_handle_t *nbio             = (nbio_handle_t*)data; 
   struct nbio_image_handle *image = nbio ? 
      (struct nbio_image_handle*)nbio->data : NULL;

   if (!image)
      goto error;

   handle = image_transfer_new(image->type);

   if (!handle)
      goto error;
 
   image->handle = handle;
   image->size   = len;
   image->cb     = &cb_image_menu_thumbnail;

   return cb_nbio_generic(nbio, &len);

error:
   return -1;
}

bool task_image_load_handler(retro_task_t *task)
{
   nbio_handle_t            *nbio  = (nbio_handle_t*)task->state;
   struct nbio_image_handle *image = (struct nbio_image_handle*)nbio->data;

   if (image)
   {
      switch (image->status)
      {
         case IMAGE_STATUS_PROCESS_TRANSFER:
            if (task_image_iterate_process_transfer(image) == -1)
               image->status = IMAGE_STATUS_PROCESS_TRANSFER_PARSE;
            break;
         case IMAGE_STATUS_TRANSFER_PARSE:
            if (image->handle && image->cb)
            {
               size_t len = 0;
               image->cb(nbio, len);
            }
            if (image->is_blocking_on_processing)
               image->status = IMAGE_STATUS_PROCESS_TRANSFER;
            break;
         case IMAGE_STATUS_TRANSFER:
            if (!image->is_blocking)
               if (task_image_iterate_transfer(image) == -1)
                  image->status = IMAGE_STATUS_TRANSFER_PARSE;
            break;
         case IMAGE_STATUS_PROCESS_TRANSFER_PARSE:
            if (image->handle && image->cb)
            {
               size_t len = 0;
               image->cb(nbio, len);
            }
            if (!image->is_finished)
               break;
         case IMAGE_STATUS_TRANSFER_PARSE_FREE:
         case IMAGE_STATUS_POLL:
         default:
            break;
      }
   }

   if (     nbio->is_finished
         && (image && image->is_finished )
         && (!task_get_cancelled(task)))
   {
      void *data = malloc(sizeof(image->ti));

      if (data)
         memcpy(data, &image->ti, sizeof(image->ti));

      task_set_data(task, data);

      return false;
   }

   return true;
}

bool task_push_image_load(const char *fullpath, retro_task_callback_t cb, void *user_data)
{
   nbio_handle_t             *nbio   = NULL;
   struct nbio_image_handle   *image = NULL;
   retro_task_t                   *t = (retro_task_t*)calloc(1, sizeof(*t));

   if (!t)
      goto error_msg;

   nbio = (nbio_handle_t*)calloc(1, sizeof(*nbio));

   if (!nbio)
      goto error;

   strlcpy(nbio->path, fullpath, sizeof(nbio->path));

   if (video_driver_supports_rgba())
      BIT32_SET(nbio->status_flags, NBIO_FLAG_IMAGE_SUPPORTS_RGBA);

   image              = (struct nbio_image_handle*)calloc(1, sizeof(*image));   
   if (!image)
      goto error;

   nbio->type         = NBIO_TYPE_NONE;
   image->type        = IMAGE_TYPE_NONE;

   if (strstr(fullpath, file_path_str(FILE_PATH_PNG_EXTENSION)))
   {
      nbio->type      = NBIO_TYPE_PNG;
      image->type     = IMAGE_TYPE_PNG;
   }
   else if (strstr(fullpath, file_path_str(FILE_PATH_JPEG_EXTENSION)) 
         || strstr(fullpath, file_path_str(FILE_PATH_JPG_EXTENSION)))
   {
      nbio->type      = NBIO_TYPE_JPEG;
      image->type     = IMAGE_TYPE_JPEG;
   }
   else if (strstr(fullpath, file_path_str(FILE_PATH_BMP_EXTENSION)))
   {
      nbio->type      = NBIO_TYPE_BMP;
      image->type     = IMAGE_TYPE_BMP;
   }
   else if (strstr(fullpath, file_path_str(FILE_PATH_TGA_EXTENSION)))
   {
      nbio->type      = NBIO_TYPE_TGA;
      image->type     = IMAGE_TYPE_TGA;
   }

   image->status      = IMAGE_STATUS_TRANSFER;

   nbio->data         = (struct nbio_image_handle*)image;
   nbio->is_finished  = false;
   nbio->cb           = &cb_nbio_image_menu_thumbnail;
   nbio->status       = NBIO_STATUS_INIT;


   t->state           = nbio;
   t->handler         = task_file_load_handler;
   t->cleanup         = task_image_load_free;
   t->callback        = cb;
   t->user_data       = user_data;

   task_queue_ctl(TASK_QUEUE_CTL_PUSH, t);

   return true;

error:
   task_image_load_free(t);
   free(t);
   if (nbio)
      free(nbio);

error_msg:
   RARCH_ERR("[image load] Failed to open '%s': %s.\n",
         fullpath, strerror(errno));

   return false;
}
