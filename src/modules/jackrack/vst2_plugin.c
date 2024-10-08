/*
 * JACK Rack
 *
 * Original:
 * Copyright (C) Robert Ham 2002, 2003 (node@users.sourceforge.net)
 *
 * Modification for MLT:
 * Copyright (C) 2024 Meltytech, LLC
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 675 Mass Ave, Cambridge, MA 02139, USA.
 */

#include <ctype.h>
#include <dlfcn.h>
#include <ladspa.h>
#include <stdio.h>
#include <stdlib.h>

#include <glib.h>

#include "framework/mlt_log.h"
#include "modules/jackrack/lock_free_fifo.h"
#include "vst2_context.h"
#include "vst2_plugin.h"
#include "vst2_process.h"

#define CONTROL_FIFO_SIZE 128

#ifdef WITH_JACK
/* swap over the jack ports in two plugins */
static void vst2_plugin_swap_aux_ports(vst2_plugin_t *plugin, vst2_plugin_t *other)
{
    guint copy;
    jack_port_t **aux_ports_tmp;

    for (copy = 0; copy < plugin->copies; copy++) {
        aux_ports_tmp = other->holders[copy].aux_ports;
        other->holders[copy].aux_ports = plugin->holders[copy].aux_ports;
        plugin->holders[copy].aux_ports = aux_ports_tmp;
    }
}
#endif

/** connect up the ladspa instance's input buffers to the previous
    plugin's audio memory.  make sure to check that plugin->prev
    exists. */
void vst2_plugin_connect_input_ports(vst2_plugin_t *plugin, LADSPA_Data **inputs)
{
    gint copy;
    unsigned long channel;
    unsigned long rack_channel;

    if (!plugin || !inputs)
        return;

    rack_channel = 0;
    for (copy = 0; copy < plugin->copies; copy++) {
        for (channel = 0; channel < plugin->desc->channels; channel++) {
            plugin->holders[copy]
                .effect->setParameter(plugin->holders[copy].effect,
                                      plugin->desc->audio_input_port_indicies[channel]
                                          - (plugin->holders[copy].effect->numInputs
                                             + plugin->holders[copy].effect->numOutputs),
                                      *inputs[rack_channel]);
            rack_channel++;
        }
    }

    plugin->audio_input_memory = inputs;
}

/** connect up a plugin's output ports to its own audio_output_memory output memory */
void vst2_plugin_connect_output_ports(vst2_plugin_t *plugin)
{
    gint copy;
    unsigned long channel;
    unsigned long rack_channel = 0;

    if (!plugin)
        return;

    for (copy = 0; copy < plugin->copies; copy++) {
        for (channel = 0; channel < plugin->desc->channels; channel++) {
            /* WIP: It might be not used */
            plugin->holders[copy]
                .effect->setParameter(plugin->holders[copy].effect,
                                      plugin->desc->audio_input_port_indicies[channel]
                                          - (plugin->holders[copy].effect->numInputs
                                             + plugin->holders[copy].effect->numOutputs),
                                      *plugin->audio_output_memory[rack_channel]);
            rack_channel++;
        }
    }
}

void vst2_process_add_plugin(vst2_process_info_t *procinfo, vst2_plugin_t *plugin)
{
    /* sort out list pointers */
    plugin->next = NULL;
    plugin->prev = procinfo->chain_end;

    if (procinfo->chain_end)
        procinfo->chain_end->next = plugin;
    else
        procinfo->chain = plugin;

    procinfo->chain_end = plugin;
}

/** remove a plugin from the chain */
vst2_plugin_t *vst2_process_remove_plugin(vst2_process_info_t *procinfo, vst2_plugin_t *plugin)
{
    /* sort out chain pointers */
    if (plugin->prev)
        plugin->prev->next = plugin->next;
    else
        procinfo->chain = plugin->next;

    if (plugin->next)
        plugin->next->prev = plugin->prev;
    else
        procinfo->chain_end = plugin->prev;

#ifdef WITH_JACK
    /* sort out the aux ports */
    if (procinfo->jack_client && plugin->desc->aux_channels > 0) {
        vst2_plugin_t *other;

        for (other = plugin->next; other; other = other->next)
            if (other->desc->id == plugin->desc->id)
                vst2_plugin_swap_aux_ports(plugin, other);
    }
#endif

    return plugin;
}

/** enable/disable a plugin */
void vst2_process_ablise_plugin(vst2_process_info_t *procinfo,
                                vst2_plugin_t *plugin,
                                gboolean enable)
{
    plugin->enabled = enable;
}

/** enable/disable a plugin */
void vst2_process_ablise_vst2_plugin_wet_dry(vst2_process_info_t *procinfo,
                                             vst2_plugin_t *plugin,
                                             gboolean enable)
{
    plugin->wet_dry_enabled = enable;
}

/** move a plugin up or down one place in the chain */
void vst2_process_move_plugin(vst2_process_info_t *procinfo, vst2_plugin_t *plugin, gint up)
{
    /* other plugins in the chain */
    vst2_plugin_t *pp = NULL, *p, *n, *nn = NULL;

    /* note that we should never receive an illogical move request
     ie, there will always be at least 1 plugin before for an up
     request or 1 plugin after for a down request */

    /* these are pointers to the plugins surrounding the specified one:
     { pp, p, plugin, n, nn } which makes things much clearer than
     tptr, tptr2 etc */
    p = plugin->prev;
    if (p)
        pp = p->prev;
    n = plugin->next;
    if (n)
        nn = n->next;

    if (up) {
        if (!p)
            return;

        if (pp)
            pp->next = plugin;
        else
            procinfo->chain = plugin;

        p->next = n;
        p->prev = plugin;

        plugin->prev = pp;
        plugin->next = p;

        if (n)
            n->prev = p;
        else
            procinfo->chain_end = p;

    } else {
        if (!n)
            return;

        if (p)
            p->next = n;
        else
            procinfo->chain = n;

        n->prev = p;
        n->next = plugin;

        plugin->prev = n;
        plugin->next = nn;

        if (nn)
            nn->prev = plugin;
        else
            procinfo->chain_end = plugin;
    }

#ifdef WITH_JACK
    if (procinfo->jack_client && plugin->desc->aux_channels > 0) {
        vst2_plugin_t *other;
        other = up ? plugin->next : plugin->prev;

        /* swap around the jack ports */
        if (other->desc->id == plugin->desc->id)
            vst2_plugin_swap_aux_ports(plugin, other);
    }
#endif
}

/** exchange an existing plugin for a newly created one */
vst2_plugin_t *vst2_process_change_plugin(vst2_process_info_t *procinfo,
                                          vst2_plugin_t *plugin,
                                          vst2_plugin_t *new_plugin)
{
    new_plugin->next = plugin->next;
    new_plugin->prev = plugin->prev;

    if (plugin->prev)
        plugin->prev->next = new_plugin;
    else
        procinfo->chain = new_plugin;

    if (plugin->next)
        plugin->next->prev = new_plugin;
    else
        procinfo->chain_end = new_plugin;

#ifdef WITH_JACK
    /* sort out the aux ports */
    if (procinfo->jack_client && plugin->desc->aux_channels > 0) {
        vst2_plugin_t *other;

        for (other = plugin->next; other; other = other->next)
            if (other->desc->id == plugin->desc->id)
                vst2_plugin_swap_aux_ports(plugin, other);
    }
#endif

    return plugin;
}

/******************************************
 ************* non RT stuff ***************
 ******************************************/

static int vst2_plugin_open_plugin(vst2_plugin_desc_t *desc,
                                   void **dl_handle_ptr,
                                   const AEffect **effect_ptr)
{
    /* void * dl_handle; */
    /* const char * dlerr; */
    //LADSPA_Descriptor_Function get_descriptor;

    /* clear the error report */
    //dlerror ();

    /* open the object file */
    //dl_handle = dlopen (desc->object_file, RTLD_NOW);
    /* dlerr = dlerror ();
     if (!dl_handle || dlerr)
       {
         if (!dlerr)
             dlerr = "unknown error";
         mlt_log_warning( NULL, "%s: error opening shared object file '%s': %s\n",
                  __FUNCTION__, desc->object_file, dlerr);
         return 1;
       } */

    /* get the get_descriptor function */
    /* get_descriptor = (LADSPA_Descriptor_Function)
       dlsym (dl_handle, "ladspa_descriptor");
     dlerr = dlerror();
     if (dlerr)
       {
         if (!dlerr)
             dlerr = "unknown error";
         mlt_log_warning( NULL, "%s: error finding descriptor symbol in object file '%s': %s\n",
                  __FUNCTION__, desc->object_file, dlerr);
         dlclose (dl_handle);
         return 1;
       } */

    /* #ifdef __APPLE__
     if (!get_descriptor (desc->index)) {
       void (*constructor)(void) = dlsym (dl_handle, "_init");
       if (constructor) constructor();
     }
   #endif */

    *effect_ptr = desc->effect;
    if (!*effect_ptr) {
        mlt_log_warning(NULL,
                        "%s: error finding index %lu in object file '%s'\n",
                        __FUNCTION__,
                        desc->index,
                        desc->object_file);
        /* dlclose (dl_handle); */
        return 1;
    }
    /* *dl_handle_ptr = dl_handle; */

    return 0;
}

static int vst2_plugin_instantiate(AEffect *effect,
                                   unsigned long vst2_plugin_index,
                                   gint copies,
                                   AEffect **effects)
{
    gint i;

    for (i = 0; i < copies; i++) {
        effects[i] = effect;
        effect->dispatcher(effect, effSetSampleRate, 0, 0, NULL, (float) vst2_sample_rate);

        /* if (!effects[i])
           {
             unsigned long d;
         
             for (d = 0; d < i; d++)
               descriptor->cleanup (effects[d]);
             
             return 1;
           } */
    }

    return 0;
}

#ifdef WITH_JACK

static void vst2_plugin_create_aux_ports(vst2_plugin_t *plugin,
                                         guint copy,
                                         vst2_context_t *vst2_context)
{
    vst2_plugin_desc_t *desc;
    //  vst2_plugin_slot_t * slot;
    unsigned long aux_channel = 1;
    unsigned long vst2_plugin_index = 1;
    unsigned long i;
    char port_name[64];
    char *vst2_plugin_name;
    char *ptr;
    //  GList * list;
    vst2_holder_t *holder;

    desc = plugin->desc;
    holder = plugin->holders + copy;

    holder->aux_ports = g_malloc(sizeof(jack_port_t *) * desc->aux_channels);

    /* make the plugin name jack worthy */
    ptr = vst2_plugin_name = g_strndup(plugin->desc->name, 7);
    while (*ptr != '\0') {
        if (*ptr == ' ')
            *ptr = '_';
        else
            *ptr = tolower(*ptr);

        ptr++;
    }

    /*	
  for (list = vst2_context->slots; list; list = g_list_next (list))
    {
      slot = (vst2_plugin_slot_t *) list->data;
      
      if (slot->plugin->desc->id == plugin->desc->id)
        vst2_plugin_index++;
    }
*/

    for (i = 0; i < desc->aux_channels; i++, aux_channel++) {
        sprintf(port_name,
                "%s_%ld-%d_%c%ld",
                vst2_plugin_name,
                vst2_plugin_index,
                copy + 1,
                desc->aux_are_input ? 'i' : 'o',
                aux_channel);

        holder->aux_ports[i] = jack_port_register(vst2_context->procinfo->jack_client,
                                                  port_name,
                                                  JACK_DEFAULT_AUDIO_TYPE,
                                                  desc->aux_are_input ? JackPortIsInput
                                                                      : JackPortIsOutput,
                                                  0);

        if (!holder->aux_ports[i]) {
            mlt_log_panic(NULL, "Could not register jack port '%s'; aborting\n", port_name);
        }
    }

    g_free(vst2_plugin_name);
}

#endif

static void vst2_plugin_init_holder(vst2_plugin_t *plugin,
                                    guint copy,
                                    AEffect *effect,
                                    vst2_context_t *vst2_context)
{
    unsigned long i;
    vst2_plugin_desc_t *desc;
    vst2_holder_t *holder;

    desc = plugin->desc;
    holder = plugin->holders + copy;

    holder->effect = effect;

    if (desc->control_port_count > 0) {
        holder->ui_control_fifos = g_malloc(sizeof(lff_t) * desc->control_port_count);
        holder->control_memory = g_malloc(sizeof(LADSPA_Data) * desc->control_port_count);
    } else {
        holder->ui_control_fifos = NULL;
        holder->control_memory = NULL;
    }

    for (i = 0; i < desc->control_port_count; i++) {
        lff_init(holder->ui_control_fifos + i, CONTROL_FIFO_SIZE, sizeof(LADSPA_Data));
        holder->control_memory[i]
            = vst2_plugin_desc_get_default_control_value(desc,
                                                         desc->control_port_indicies[i],
                                                         vst2_sample_rate);
        holder->effect->setParameter(holder->effect,
                                     desc->control_port_indicies[i]
                                         - (holder->effect->numInputs + holder->effect->numOutputs),
                                     *(holder->control_memory + i));
    }

    if (desc->status_port_count > 0) {
        holder->status_memory = g_malloc(sizeof(LADSPA_Data) * desc->status_port_count);
    } else {
        holder->status_memory = NULL;
    }

    if (holder->control_memory) {
        for (i = 0; i < desc->status_port_count; i++) {
            holder->effect->setParameter(holder->effect,
                                         desc->control_port_indicies[i]
                                             - (holder->effect->numInputs
                                                + holder->effect->numOutputs),
                                         *(holder->control_memory + i));
        }
    }

#ifdef WITH_JACK
    if (vst2_context->procinfo->jack_client && plugin->desc->aux_channels > 0)
        vst2_plugin_create_aux_ports(plugin, copy, vst2_context);
#endif

    /* if (plugin->descriptor->activate)
       plugin->descriptor->activate (effects); */
}

vst2_plugin_t *vst2_plugin_new(vst2_plugin_desc_t *desc, vst2_context_t *vst2_context)
{
    void *dl_handle;
    //const LADSPA_Descriptor * descriptor;
    const AEffect *effect;
    AEffect **effects;
    gint copies;
    unsigned long i;
    int err;
    vst2_plugin_t *plugin;

    /* open the plugin */
    err = vst2_plugin_open_plugin(desc, &dl_handle, &effect);
    if (err)
        return NULL;

    /* create the effects */
    copies = vst2_plugin_desc_get_copies(desc, vst2_context->channels);
    effects = g_malloc(sizeof(AEffect) * copies);

    err = vst2_plugin_instantiate(desc->effect, desc->index, copies, effects);
    if (err) {
        g_free(effects);
        dlclose(dl_handle);
        return NULL;
    }

    plugin = g_malloc(sizeof(vst2_plugin_t));

    plugin->dl_handle = dl_handle;
    plugin->desc = desc;
    plugin->copies = copies;
    plugin->enabled = FALSE;
    plugin->next = NULL;
    plugin->prev = NULL;
    plugin->wet_dry_enabled = FALSE;
    plugin->vst2_context = vst2_context;

    /* create audio memory and wet/dry stuff */
    plugin->audio_output_memory = g_malloc(sizeof(LADSPA_Data *) * vst2_context->channels);
    plugin->wet_dry_fifos = g_malloc(sizeof(lff_t) * vst2_context->channels);
    plugin->wet_dry_values = g_malloc(sizeof(LADSPA_Data) * vst2_context->channels);

    for (i = 0; i < vst2_context->channels; i++) {
        plugin->audio_output_memory[i] = g_malloc(sizeof(LADSPA_Data) * vst2_buffer_size);
        lff_init(plugin->wet_dry_fifos + i, CONTROL_FIFO_SIZE, sizeof(LADSPA_Data));
        plugin->wet_dry_values[i] = 1.0;
    }

    /* create holders and fill them out */
    plugin->holders = g_malloc(sizeof(vst2_holder_t) * copies);
    for (i = 0; i < copies; i++)
        vst2_plugin_init_holder(plugin, i, effects[i], vst2_context);

    return plugin;
}

void vst2_plugin_destroy(vst2_plugin_t *plugin)
{
    unsigned long i, j;
    int err;

    /* destroy holders */
    for (i = 0; i < plugin->copies; i++) {
        if (plugin->desc->control_port_count > 0) {
            for (j = 0; j < plugin->desc->control_port_count; j++) {
                lff_free(plugin->holders[i].ui_control_fifos + j);
            }
            g_free(plugin->holders[i].ui_control_fifos);
            g_free(plugin->holders[i].control_memory);
        }

        if (plugin->desc->status_port_count > 0) {
            g_free(plugin->holders[i].status_memory);
        }

#ifdef WITH_JACK
        /* aux ports */
        if (plugin->vst2_context->procinfo->jack_client && plugin->desc->aux_channels > 0) {
            for (j = 0; j < plugin->desc->aux_channels; j++) {
                err = jack_port_unregister(plugin->vst2_context->procinfo->jack_client,
                                           plugin->holders[i].aux_ports[j]);

                if (err)
                    mlt_log_warning(NULL, "%s: could not unregister jack port\n", __FUNCTION__);
            }

            g_free(plugin->holders[i].aux_ports);
        }
#endif
    }

    g_free(plugin->holders);

    for (i = 0; i < plugin->vst2_context->channels; i++) {
        g_free(plugin->audio_output_memory[i]);
        lff_free(plugin->wet_dry_fifos + i);
    }

    g_free(plugin->audio_output_memory);
    g_free(plugin->wet_dry_fifos);
    g_free(plugin->wet_dry_values);

    err = dlclose(plugin->dl_handle);
    if (err) {
        mlt_log_warning(NULL,
                        "%s: error closing shared object '%s': %s\n",
                        __FUNCTION__,
                        plugin->desc->object_file,
                        dlerror());
    }

    g_free(plugin);
}

/* EOF */
