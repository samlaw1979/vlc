/*****************************************************************************
 * dshow.c : DirectShow access module for vlc
 *****************************************************************************
 * Copyright (C) 2002 VideoLAN
 * $Id: dshow.cpp,v 1.3 2003/08/25 22:57:40 gbazin Exp $
 *
 * Author: Gildas Bazin <gbazin@netcourrier.com>
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
 * Foundation, Inc., 59 Temple Place - Suite 330, Boston, MA  02111, USA.
 *****************************************************************************/

/*****************************************************************************
 * Preamble
 *****************************************************************************/
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include <vlc/vlc.h>
#include <vlc/input.h>
#include <vlc/vout.h>

#ifndef _MSC_VER
#   include <wtypes.h>
#   include <unknwn.h>
#   include <ole2.h>
#   include <limits.h>
#   define _WINGDI_ 1
#   define AM_NOVTABLE
#   define _OBJBASE_H_
#   undef _X86_
#   define _I64_MAX LONG_LONG_MAX
#   define LONGLONG long long
#endif

#include <dshow.h>

#include "filter.h"

/*****************************************************************************
 * Local prototypes
 *****************************************************************************/
static int  AccessOpen  ( vlc_object_t * );
static void AccessClose ( vlc_object_t * );
static int  Read        ( input_thread_t *, byte_t *, size_t );

static int  DemuxOpen  ( vlc_object_t * );
static void DemuxClose ( vlc_object_t * );
static int  Demux      ( input_thread_t * );

static int OpenDevice( input_thread_t *, string, vlc_bool_t );
static IBaseFilter *FindCaptureDevice( vlc_object_t *, string *,
                                       list<string> *, vlc_bool_t );
static bool ConnectFilters( IFilterGraph *p_graph, IBaseFilter *p_filter,
                            IPin *p_input_pin );

/*****************************************************************************
 * Module descriptior
 *****************************************************************************/
#define CACHING_TEXT N_("Caching value in ms")
#define CACHING_LONGTEXT N_( \
    "Allows you to modify the default caching value for directshow streams. " \
    "This value should be set in miliseconds units." )

vlc_module_begin();
    set_description( _("DirectShow input") );
    add_category_hint( N_("dshow"), NULL, VLC_TRUE );
    add_integer( "dshow-caching", DEFAULT_PTS_DELAY / 1000, NULL,
                 CACHING_TEXT, CACHING_LONGTEXT, VLC_TRUE );
    add_shortcut( "dshow" );
    set_capability( "access", 0 );
    set_callbacks( AccessOpen, AccessClose );

    add_submodule();
    set_description( _("DirectShow demuxer") );
    add_shortcut( "dshow" );
    set_capability( "demux", 200 );
    set_callbacks( DemuxOpen, DemuxClose );

vlc_module_end();

/****************************************************************************
 * I. Access Part
 ****************************************************************************/

/*
 * header:
 *  fcc  ".dsh"
 *  u32    stream count
 *      fcc "auds"|"vids"       0
 *      fcc codec               4
 *      if vids
 *          u32 width           8
 *          u32 height          12
 *          u32 padding         16
 *      if auds
 *          u32 channels        12
 *          u32 samplerate      8
 *          u32 samplesize      16
 *
 * data:
 *  u32     stream number
 *  u32     data size
 *  u8      data
 */

static void SetDWBE( uint8_t *p, uint32_t dw )
{
    p[0] = (dw >> 24)&0xff;
    p[1] = (dw >> 16)&0xff;
    p[2] = (dw >>  8)&0xff;
    p[3] = (dw      )&0xff;
}

static void SetQWBE( uint8_t *p, uint64_t qw )
{
    SetDWBE( p, (qw >> 32)&0xffffffff );
    SetDWBE( &p[4], qw&0xffffffff );
}

/****************************************************************************
 * DirectShow elementary stream descriptor
 ****************************************************************************/
typedef struct dshow_stream_t
{
    string          devicename;
    IBaseFilter     *p_device_filter;
    CaptureFilter   *p_capture_filter;
    AM_MEDIA_TYPE   mt;
    int             i_fourcc;

    union
    {
      VIDEOINFOHEADER video;
      WAVEFORMATEX    audio;

    } header;

    VLCMediaSample  sample;
    int             i_data_size;
    int             i_data_pos;
    uint8_t         *p_data;

} dshow_stream_t;

/****************************************************************************
 * Access descriptor declaration
 ****************************************************************************/
struct access_sys_t
{
    IFilterGraph  *p_graph;
    IMediaControl *p_control;

    /* header */
    int     i_header_size;
    int     i_header_pos;
    uint8_t *p_header;

    /* list of elementary streams */
    dshow_stream_t **pp_streams;
    int            i_streams;
    int            i_current_stream;
};

/*****************************************************************************
 * Open: open direct show device
 *****************************************************************************/
static int AccessOpen( vlc_object_t *p_this )
{
    input_thread_t *p_input = (input_thread_t *)p_this;
    access_sys_t   *p_sys;

    /* parse url and open device(s) */
    char *psz_dup, *psz_parser;
    psz_dup = strdup( p_input->psz_name );
    psz_parser = psz_dup;
    string vdevname, adevname;

    while( *psz_parser && *psz_parser != ':' )
    {
        psz_parser++;
    }

    if( *psz_parser == ':' )
    {
        /* read options */
        for( ;; )
        {
            int i_len;

            *psz_parser++ = '\0';
            if( !strncmp( psz_parser, "vdev=", strlen( "vdev=" ) ) )
            {
                psz_parser += strlen( "vdev=" );
                if( strchr( psz_parser, ':' ) )
                {
                    i_len = strchr( psz_parser, ':' ) - psz_parser;
                }
                else
                {
                    i_len = strlen( psz_parser );
                }

                vdevname = string( psz_parser, i_len );

                psz_parser += i_len;
            }
            else if( !strncmp( psz_parser, "adev=", strlen( "adev=" ) ) )
            {
                psz_parser += strlen( "adev=" );
                if( strchr( psz_parser, ':' ) )
                {
                    i_len = strchr( psz_parser, ':' ) - psz_parser;
                }
                else
                {
                    i_len = strlen( psz_parser );
                }

                adevname = string( psz_parser, i_len );

                psz_parser += i_len;
            }
            else
            {
                msg_Warn( p_input, "unknown option" );
            }

            while( *psz_parser && *psz_parser != ':' )
            {
                psz_parser++;
            }

            if( *psz_parser == '\0' )
            {
                break;
            }
        }
    }

    free( psz_dup );

    p_input->pf_read        = Read;
    p_input->pf_seek        = NULL;
    p_input->pf_set_area    = NULL;
    p_input->pf_set_program = NULL;

    vlc_mutex_lock( &p_input->stream.stream_lock );
    p_input->stream.b_pace_control = 0;
    p_input->stream.b_seekable = 0;
    p_input->stream.p_selected_area->i_size = 0;
    p_input->stream.p_selected_area->i_tell = 0;
    p_input->stream.i_method = INPUT_METHOD_FILE;
    vlc_mutex_unlock( &p_input->stream.stream_lock );
    p_input->i_pts_delay = config_GetInt( p_input, "dshow-caching" ) * 1000;

    /* Initialize OLE/COM */
    CoInitializeEx( 0, COINIT_APARTMENTTHREADED );

    /* create access private data */
    p_input->p_access_data = p_sys =
        (access_sys_t *)malloc( sizeof( access_sys_t ) );

    /* Initialize some data */
    p_sys->i_streams = 0;
    p_sys->pp_streams = (dshow_stream_t **)malloc( 1 );

    /* Create header */
    p_sys->i_header_size = 8;
    p_sys->p_header      = (uint8_t *)malloc( p_sys->i_header_size );
    memcpy(  &p_sys->p_header[0], ".dsh", 4 );
    SetDWBE( &p_sys->p_header[4], 1 );
    p_sys->i_header_pos = p_sys->i_header_size;

    /* Build directshow graph */
    CoCreateInstance( CLSID_FilterGraph, 0, CLSCTX_INPROC,
                      (REFIID)IID_IFilterGraph, (void **)&p_sys->p_graph );

    p_sys->p_graph->QueryInterface( IID_IMediaControl,
                                    (void **)&p_sys->p_control );

    if( OpenDevice( p_input, vdevname, 0 ) != VLC_SUCCESS )
    {
        msg_Err( p_input, "can't open video");
    }

    if( OpenDevice( p_input, adevname, 1 ) != VLC_SUCCESS )
    {
        msg_Err( p_input, "can't open audio");
    }

    if( !p_sys->i_streams )
    {
        /* Uninitialize OLE/COM */
        CoUninitialize();   

        /* Release directshow objects */
        p_sys->p_control->Release();
        p_sys->p_graph->Release();
        free( p_sys->p_header );
        free( p_sys );
        return VLC_EGENERIC;
    }

    /* Initialize some data */
    p_sys->i_current_stream = 0;
    p_sys->i_header_pos = 0;

    /* Everything is ready. Let's rock baby */
    p_sys->p_control->Run();

    return VLC_SUCCESS;
}

/*****************************************************************************
 * AccessClose: close device
 *****************************************************************************/
static void AccessClose( vlc_object_t *p_this )
{
    input_thread_t *p_input = (input_thread_t *)p_this;
    access_sys_t    *p_sys  = p_input->p_access_data;

    /* Stop capturing stuff */
    p_sys->p_control->Stop();
    p_sys->p_control->Release();

#if 0
    /* Remove filters from graph */
    for( int i = 0; i < p_sys->i_streams; i++ )
    {
        p_sys->p_graph->RemoveFilter( p_sys->pp_streams[i]->p_device_filter );
        p_sys->p_graph->RemoveFilter( p_sys->pp_streams[i]->p_capture_filter );
        p_sys->pp_streams[i]->p_device_filter->Release();
        p_sys->pp_streams[i]->p_capture_filter->Release();
    }
    p_sys->p_graph->Release();
#endif

    /* Uninitialize OLE/COM */
    CoUninitialize();   

    free( p_sys->p_header );
    for( int i = 0; i < p_sys->i_streams; i++ ) delete p_sys->pp_streams[i];
    free( p_sys->pp_streams );
    free( p_sys );
}

/****************************************************************************
 * ConnectFilters
 ****************************************************************************/
static bool ConnectFilters( IFilterGraph *p_graph, IBaseFilter *p_filter,
                            IPin *p_input_pin )
{
    IEnumPins *p_enumpins;
    IPin *p_output_pin;
    ULONG i_fetched;

    if( S_OK != p_filter->EnumPins( &p_enumpins ) ) return false;

    while( S_OK == p_enumpins->Next( 1, &p_output_pin, &i_fetched ) )
    {
        if( S_OK == p_graph->ConnectDirect( p_output_pin, p_input_pin, 0 ) )
        {
            p_enumpins->Release();
            return true;
        }
    }

    p_enumpins->Release();
    return false;
}

static int OpenDevice( input_thread_t *p_input, string devicename,
                       vlc_bool_t b_audio )
{
    access_sys_t *p_sys = p_input->p_access_data;
    list<string> list_devices;

    /* Enumerate audio devices and display their names */
    FindCaptureDevice( (vlc_object_t *)p_input, NULL, &list_devices, b_audio );

    list<string>::iterator iter;
    for( iter = list_devices.begin(); iter != list_devices.end(); iter++ )
        msg_Dbg( p_input, "found device: %s", iter->c_str() );

    /* If no device name was specified, pick the 1st one */
    if( devicename.size() == 0 )
    {
        devicename = *list_devices.begin();
    }

    // Use the system device enumerator and class enumerator to find
    // a capture/preview device, such as a desktop USB video camera.
    IBaseFilter *p_device_filter =
        FindCaptureDevice( (vlc_object_t *)p_input, &devicename,
                           NULL, b_audio );
    if( p_device_filter )
        msg_Dbg( p_input, "using device: %s", devicename.c_str() );
    else
    {
        msg_Err( p_input, "can't use device: %s", devicename.c_str() );
        return VLC_EGENERIC;
    }

    /* Create and add our capture filter */
    CaptureFilter *p_capture_filter = new CaptureFilter( p_input );
    p_sys->p_graph->AddFilter( p_capture_filter, 0 );

    /* Add the device filter to the graph (seems necessary with VfW before
     * accessing pin attributes). */
    p_sys->p_graph->AddFilter( p_device_filter, 0 );

    /* Attempt to connect one of this device's capture output pins */
    msg_Dbg( p_input, "connecting filters" );
    if( ConnectFilters( p_sys->p_graph, p_device_filter,
                        p_capture_filter->CustomGetPin() ) )
    {
        /* Success */
        dshow_stream_t dshow_stream;
        dshow_stream.mt =
            p_capture_filter->CustomGetPin()->CustomGetMediaType();

        if( dshow_stream.mt.majortype == MEDIATYPE_Video )
        {
            msg_Dbg( p_input, "MEDIATYPE_Video");

            if( dshow_stream.mt.subtype == MEDIASUBTYPE_RGB8 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'G', 'R', 'E', 'Y' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_RGB555 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'R', 'V', '1', '5' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_RGB565 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'R', 'V', '1', '6' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_RGB24 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'R', 'V', '2', '4' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_RGB32 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'R', 'V', '3', '2' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_ARGB32 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'R', 'G', 'B', 'A' );
            
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_YUYV )
                dshow_stream.i_fourcc = VLC_FOURCC( 'Y', 'U', 'Y', 'V' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_Y411 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'I', '4', '1', 'N' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_Y41P )
                dshow_stream.i_fourcc = VLC_FOURCC( 'I', '4', '1', '1' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_YUY2 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'Y', 'U', 'Y', '2' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_YVYU )
                dshow_stream.i_fourcc = VLC_FOURCC( 'Y', 'V', 'Y', 'U' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_Y411 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'I', '4', '1', 'N' );
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_YV12 )
                dshow_stream.i_fourcc = VLC_FOURCC( 'Y', 'V', '1', '2' );
            else goto fail;

            dshow_stream.header.video =
                *(VIDEOINFOHEADER *)dshow_stream.mt.pbFormat;

            /* Add video stream to header */
            p_sys->i_header_size += 20;
            p_sys->p_header = (uint8_t *)realloc( p_sys->p_header,
                                                  p_sys->i_header_size );
            memcpy(  &p_sys->p_header[p_sys->i_header_pos], "vids", 4 );
            memcpy(  &p_sys->p_header[p_sys->i_header_pos + 4],
                     &dshow_stream.i_fourcc, 4 );
            SetDWBE( &p_sys->p_header[p_sys->i_header_pos + 8],
                     dshow_stream.header.video.bmiHeader.biWidth );
            SetDWBE( &p_sys->p_header[p_sys->i_header_pos + 12],
                     dshow_stream.header.video.bmiHeader.biHeight );
            SetDWBE( &p_sys->p_header[p_sys->i_header_pos + 16], 0 );
            p_sys->i_header_pos = p_sys->i_header_size;
        }

        else if( dshow_stream.mt.majortype == MEDIATYPE_Audio &&
                 dshow_stream.mt.formattype == FORMAT_WaveFormatEx )
        {
            msg_Dbg( p_input, "MEDIATYPE_Audio");

            if( dshow_stream.mt.subtype == MEDIASUBTYPE_PCM )
                dshow_stream.i_fourcc = VLC_FOURCC( 'a', 'r', 'a', 'w' );
#if 0
            else if( dshow_stream.mt.subtype == MEDIASUBTYPE_IEEE_FLOAT )
                dshow_stream.i_fourcc = VLC_FOURCC( 'f', 'l', '3', '2' );
#endif
            else goto fail;

            dshow_stream.header.audio =
                *(WAVEFORMATEX *)dshow_stream.mt.pbFormat;

            /* Add audio stream to header */
            p_sys->i_header_size += 20;
            p_sys->p_header = (uint8_t *)realloc( p_sys->p_header,
                                                  p_sys->i_header_size );
            memcpy(  &p_sys->p_header[p_sys->i_header_pos], "auds", 4 );
            memcpy(  &p_sys->p_header[p_sys->i_header_pos + 4],
                     &dshow_stream.i_fourcc, 4 );
            SetDWBE( &p_sys->p_header[p_sys->i_header_pos + 8],
                     dshow_stream.header.audio.nChannels );
            SetDWBE( &p_sys->p_header[p_sys->i_header_pos + 12],
                     dshow_stream.header.audio.nSamplesPerSec );
            SetDWBE( &p_sys->p_header[p_sys->i_header_pos + 16],
                     dshow_stream.header.audio.wBitsPerSample );
            p_sys->i_header_pos = p_sys->i_header_size;
        }
        else goto fail;

        /* Add directshow elementary stream to our list */
        dshow_stream.sample.p_sample  = NULL;
        dshow_stream.i_data_size = 0;
        dshow_stream.i_data_pos = 0;
        dshow_stream.p_device_filter = p_device_filter;
        dshow_stream.p_capture_filter = p_capture_filter;

        p_sys->pp_streams =
            (dshow_stream_t **)realloc( p_sys->pp_streams,
                                        sizeof(dshow_stream_t *)
                                        * (p_sys->i_streams + 1) );
        p_sys->pp_streams[p_sys->i_streams] = new dshow_stream_t;
        *p_sys->pp_streams[p_sys->i_streams++] = dshow_stream;
        SetDWBE( &p_sys->p_header[4], (uint32_t)p_sys->i_streams );

        return VLC_SUCCESS;
    }

 fail:
    /* Remove filters from graph */
    p_sys->p_graph->RemoveFilter( p_device_filter );
    p_sys->p_graph->RemoveFilter( p_capture_filter );

    /* Release objects */
    p_device_filter->Release();
    p_capture_filter->Release();

    return VLC_EGENERIC;
}

static IBaseFilter *
FindCaptureDevice( vlc_object_t *p_this, string *p_devicename,
                   list<string> *p_listdevices, vlc_bool_t b_audio )
{
    IBaseFilter *p_base_filter = NULL;
    IMoniker *p_moniker = NULL;
    ULONG i_fetched;
    HRESULT hr;

    /* Create the system device enumerator */
    ICreateDevEnum *p_dev_enum = NULL;

    hr = CoCreateInstance( CLSID_SystemDeviceEnum, NULL, CLSCTX_INPROC,
                           IID_ICreateDevEnum, (void **)&p_dev_enum );
    if( FAILED(hr) )
    {
        msg_Err( p_this, "failed to create the device enumerator (0x%x)", hr);
        return NULL;
    }

    /* Create an enumerator for the video capture devices */
    IEnumMoniker *p_class_enum = NULL;
    if( !b_audio )
        hr = p_dev_enum->CreateClassEnumerator( CLSID_VideoInputDeviceCategory,
                                                &p_class_enum, 0 );
    else
        hr = p_dev_enum->CreateClassEnumerator( CLSID_AudioInputDeviceCategory,
                                                &p_class_enum, 0 );
    p_dev_enum->Release();
    if( FAILED(hr) )
    {
        msg_Err( p_this, "failed to create the class enumerator (0x%x)", hr );
        return NULL;
    }

    /* If there are no enumerators for the requested type, then 
     * CreateClassEnumerator will succeed, but p_class_enum will be NULL */
    if( p_class_enum == NULL )
    {
        msg_Err( p_this, "no capture device was detected." );
        return NULL;
    }

    /* Enumerate the devices */

    /* Note that if the Next() call succeeds but there are no monikers,
     * it will return S_FALSE (which is not a failure). Therefore, we check
     * that the return code is S_OK instead of using SUCCEEDED() macro. */

    while( p_class_enum->Next( 1, &p_moniker, &i_fetched ) == S_OK )
    {
        /* Getting the property page to get the device name */
        IPropertyBag *p_bag;
        hr = p_moniker->BindToStorage( 0, 0, IID_IPropertyBag,
                                       (void **)&p_bag );
        if( SUCCEEDED(hr) )
        {
            VARIANT var;
            var.vt = VT_BSTR;
            hr = p_bag->Read( L"FriendlyName", &var, NULL );
            p_bag->Release();
            if( SUCCEEDED(hr) )
            {
                int i_convert = ( lstrlenW( var.bstrVal ) + 1 ) * 2;
                char *p_buf = (char *)alloca( i_convert ); p_buf[0] = 0;
                WideCharToMultiByte( CP_ACP, 0, var.bstrVal, -1, p_buf,
                                     i_convert, NULL, NULL );
                SysFreeString(var.bstrVal);

                if( p_listdevices ) p_listdevices->push_back( p_buf );

                if( p_devicename && *p_devicename == string(p_buf) )
                {
                    /* Bind Moniker to a filter object */
                    hr = p_moniker->BindToObject( 0, 0, IID_IBaseFilter,
                                                  (void **)&p_base_filter );
                    if( FAILED(hr) )
                    {
                        msg_Err( p_this, "couldn't bind moniker to filter "
                                 "object (0x%x)", hr );
                        p_moniker->Release();
                        p_class_enum->Release();
                        return NULL;
                    }
                    p_moniker->Release();
                    p_class_enum->Release();
                    return p_base_filter;
                }
            }
        }

        p_moniker->Release();
    }

    p_class_enum->Release();
    return NULL;
}

/*****************************************************************************
 * Read: reads from the device into PES packets.
 *****************************************************************************
 * Returns -1 in case of error, 0 in case of EOF, otherwise the number of
 * bytes.
 *****************************************************************************/
static int Read( input_thread_t * p_input, byte_t * p_buffer, size_t i_len )
{
    access_sys_t   *p_sys = p_input->p_access_data;
    dshow_stream_t *p_stream = p_sys->pp_streams[p_sys->i_current_stream];
    int            i_data = 0;

#if 0
    msg_Info( p_input, "access read data_size %i, data_pos %i",
              p_sys->i_data_size, p_sys->i_data_pos );
#endif

    while( i_len > 0 )
    {
        /* First copy header if any */
        if( i_len > 0 && p_sys->i_header_pos < p_sys->i_header_size )
        {
            int i_copy;

            i_copy = __MIN( p_sys->i_header_size -
                            p_sys->i_header_pos, (int)i_len );
            memcpy( p_buffer, &p_sys->p_header[p_sys->i_header_pos], i_copy );
            p_sys->i_header_pos += i_copy;

            p_buffer += i_copy;
            i_len -= i_copy;
            i_data += i_copy;
        }

        /* Then copy stream data if any */
        if( i_len > 0 && p_stream->i_data_pos < p_stream->i_data_size )
        {
            int i_copy = __MIN( p_stream->i_data_size -
                                p_stream->i_data_pos, (int)i_len );

            memcpy( p_buffer, &p_stream->p_data[p_stream->i_data_pos],
                    i_copy );
            p_stream->i_data_pos += i_copy;

            p_buffer += i_copy;
            i_len -= i_copy;
            i_data += i_copy;
        }

        /* The caller got what he wanted */
        if( i_len <= 0 ) return i_data;

        /* Read no more than one frame at a time, otherwise we kill latency */
        if( p_stream->i_data_size && i_data &&
            p_stream->i_data_pos == p_stream->i_data_size )
        {
            p_stream->i_data_pos = p_stream->i_data_size = 0;
            return i_data;
        }

        /* Get new sample/frame from next stream */
        //if( p_sream->sample.p_sample ) p_stream->sample.p_sample->Release();
        p_sys->i_current_stream =
            (p_sys->i_current_stream + 1) % p_sys->i_streams;
        p_stream = p_sys->pp_streams[p_sys->i_current_stream];
        if( p_stream->p_capture_filter &&
            p_stream->p_capture_filter->CustomGetPin()
                ->CustomGetSample( &p_stream->sample ) == S_OK )
        {
            p_stream->i_data_pos = 0;
            p_stream->i_data_size =
                p_stream->sample.p_sample->GetActualDataLength();
            p_stream->sample.p_sample->GetPointer( &p_stream->p_data );

            REFERENCE_TIME i_pts, i_end_date;
            HRESULT hr =
                p_stream->sample.p_sample->GetTime( &i_pts, &i_end_date );
            if( hr != VFW_S_NO_STOP_TIME && hr != S_OK ) i_pts = 0;

            if( !i_pts )
            {
                /* Use our data timestamp */
                i_pts = p_stream->sample.i_timestamp;
            }

#if 0
            msg_Dbg( p_input, "Read() PTS: "I64Fd, i_pts );
#endif

            /* Create pseudo header */
            p_sys->i_header_size = 16;
            p_sys->i_header_pos  = 0;
            SetDWBE( &p_sys->p_header[0], p_sys->i_current_stream );
            SetDWBE( &p_sys->p_header[4], p_stream->i_data_size );
            SetQWBE( &p_sys->p_header[8], i_pts  * 9 / 1000 );
        }
        else msleep( 10000 );
    }

    return i_data;
}

/****************************************************************************
 * I. Demux Part
 ****************************************************************************/
static int DemuxOpen( vlc_object_t *p_this )
{
    input_thread_t *p_input = (input_thread_t *)p_this;

    uint8_t        *p_peek;
    int            i_streams;
    int            i;

    data_packet_t  *p_pk;

    /* Initialize access plug-in structures. */
    if( p_input->i_mtu == 0 )
    {
        /* Improve speed. */
        p_input->i_bufsize = INPUT_DEFAULT_BUFSIZE ;
    }

    /* a little test to see if it's a dshow stream */
    if( input_Peek( p_input, &p_peek, 8 ) < 8 )
    {
        msg_Warn( p_input, "dshow plugin discarded (cannot peek)" );
        return( VLC_EGENERIC );
    }

    if( strcmp( (const char *)p_peek, ".dsh" ) ||
        GetDWBE( &p_peek[4] ) <= 0 )
    {
        msg_Warn( p_input, "dshow plugin discarded (not a valid stream)" );
        return VLC_EGENERIC;
    }

    /*  create one program */
    vlc_mutex_lock( &p_input->stream.stream_lock );
    if( input_InitStream( p_input, 0 ) == -1)
    {
        vlc_mutex_unlock( &p_input->stream.stream_lock );
        msg_Err( p_input, "cannot init stream" );
        return( VLC_EGENERIC );
    }
    if( input_AddProgram( p_input, 0, 0) == NULL )
    {
        vlc_mutex_unlock( &p_input->stream.stream_lock );
        msg_Err( p_input, "cannot add program" );
        return( VLC_EGENERIC );
    }

    p_input->stream.p_selected_program = p_input->stream.pp_programs[0];
    p_input->stream.i_mux_rate =  0;

    i_streams = GetDWBE( &p_peek[4] );
    if( input_Peek( p_input, &p_peek, 8 + 20 * i_streams )
        < 8 + 20 * i_streams )
    {
        msg_Err( p_input, "dshow plugin discarded (cannot peek)" );
        return( VLC_EGENERIC );
    }
    p_peek += 8;

    for( i = 0; i < i_streams; i++ )
    {
        es_descriptor_t *p_es;

        if( !strncmp( (const char *)p_peek, "auds", 4 ) )
        {
#define wf ((WAVEFORMATEX*)p_es->p_waveformatex)
            p_es = input_AddES( p_input, p_input->stream.pp_programs[0],
                                i + 1, AUDIO_ES, NULL, 0 );
            p_es->i_stream_id   = i + 1;
            p_es->i_fourcc      =
                VLC_FOURCC( p_peek[4], p_peek[5], p_peek[6], p_peek[7] );

            p_es->p_waveformatex= malloc( sizeof( WAVEFORMATEX ) );

            wf->wFormatTag      = 0;//WAVE_FORMAT_UNKNOWN;
            wf->nChannels       = GetDWBE( &p_peek[8] );
            wf->nSamplesPerSec  = GetDWBE( &p_peek[12] );
            wf->wBitsPerSample  = GetDWBE( &p_peek[16] );
            wf->nBlockAlign     = wf->wBitsPerSample * wf->nChannels / 8;
            wf->nAvgBytesPerSec = wf->nBlockAlign * wf->nSamplesPerSec;
            wf->cbSize          = 0;

            msg_Dbg( p_input, "added new audio es %d channels %dHz",
                     wf->nChannels, wf->nSamplesPerSec );

            input_SelectES( p_input, p_es );
#undef wf
        }
        else if( !strncmp( (const char *)p_peek, "vids", 4 ) )
        {
#define bih ((BITMAPINFOHEADER*)p_es->p_bitmapinfoheader)
            p_es = input_AddES( p_input, p_input->stream.pp_programs[0],
                                i + 1, VIDEO_ES, NULL, 0 );
            p_es->i_stream_id   = i + 1;
            p_es->i_fourcc  =
                VLC_FOURCC( p_peek[4], p_peek[5], p_peek[6], p_peek[7] );

            p_es->p_bitmapinfoheader = malloc( sizeof( BITMAPINFOHEADER ) );

            bih->biSize     = sizeof( BITMAPINFOHEADER );
            bih->biWidth    = GetDWBE( &p_peek[8] );
            bih->biHeight   = GetDWBE( &p_peek[12] );
            bih->biPlanes   = 0;
            bih->biBitCount = 0;
            bih->biCompression      = 0;
            bih->biSizeImage= 0;
            bih->biXPelsPerMeter    = 0;
            bih->biYPelsPerMeter    = 0;
            bih->biClrUsed  = 0;
            bih->biClrImportant     = 0;

            msg_Dbg( p_input, "added new video es %4.4s %dx%d",
                     (char*)&p_es->i_fourcc, bih->biWidth, bih->biHeight );

            input_SelectES( p_input, p_es );
#undef bih
        }

        p_peek += 20;
    }

    p_input->stream.p_selected_program->b_is_ok = 1;
    vlc_mutex_unlock( &p_input->stream.stream_lock );

    if( input_SplitBuffer( p_input, &p_pk, 8 + i_streams * 20 ) > 0 )
    {
        input_DeletePacket( p_input->p_method_data, p_pk );
    }

    p_input->pf_demux = Demux;
    return VLC_SUCCESS;
}

static void DemuxClose( vlc_object_t *p_this )
{
    return;
}

static int Demux( input_thread_t *p_input )
{
    es_descriptor_t *p_es;
    pes_packet_t    *p_pes;

    int i_stream;
    int i_size;
    uint8_t *p_peek;
    mtime_t i_pcr;

    if( input_Peek( p_input, &p_peek, 16 ) < 16 )
    {
        msg_Warn( p_input, "cannot peek (EOF ?)" );
        return( 0 );
    }

    i_stream = GetDWBE( &p_peek[0] );
    i_size   = GetDWBE( &p_peek[4] );
    i_pcr    = GetQWBE( &p_peek[8] );

    //msg_Dbg( p_input, "stream=%d size=%d", i_stream, i_size );
    //p_es = input_FindES( p_input, i_stream );

    p_es = p_input->stream.p_selected_program->pp_es[i_stream];
    if( !p_es )
    {
        msg_Err( p_input, "cannot find ES" );
    }

    p_pes = input_NewPES( p_input->p_method_data );
    if( p_pes == NULL )
    {
        msg_Warn( p_input, "cannot allocate PES" );
        msleep( 1000 );
        return( 1 );
    }
    i_size += 16;
    while( i_size > 0 )
    {
        data_packet_t   *p_data;
        int i_read;

        if( (i_read = input_SplitBuffer( p_input, &p_data,
                                         __MIN( i_size, 10000 ) ) ) <= 0 )
        {
            input_DeletePES( p_input->p_method_data, p_pes );
            return( 0 );
        }
        if( !p_pes->p_first )
        {
            p_pes->p_first = p_data;
            p_pes->i_nb_data = 1;
            p_pes->i_pes_size = i_read;
        }
        else
        {
            p_pes->p_last->p_next  = p_data;
            p_pes->i_nb_data++;
            p_pes->i_pes_size += i_read;
        }
        p_pes->p_last  = p_data;
        i_size -= i_read;
    }

    p_pes->p_first->p_payload_start += 16;
    p_pes->i_pes_size               -= 16;

    if( p_es && p_es->p_decoder_fifo )
    {
        /* Call the pace control. */
        input_ClockManageRef( p_input, p_input->stream.p_selected_program,
                              i_pcr );

        p_pes->i_pts = p_pes->i_dts = i_pcr <= 0 ? 0 :
            input_ClockGetTS( p_input, p_input->stream.p_selected_program,
                              i_pcr );

        input_DecodePES( p_es->p_decoder_fifo, p_pes );
    }
    else
    {
        input_DeletePES( p_input->p_method_data, p_pes );
    }

    return 1;
}
