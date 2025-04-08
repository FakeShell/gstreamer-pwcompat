gstpipewire_sources = [
  'gstpipewire.c',
  'gstpipewirecore.c',
  'gstpipewireclock.c',
  'gstpipewireformat.c',
  'gstpipewirepool.c',
  'gstpipewiresink.c',
  'gstpipewiresrc.c',
  'gstpipewirestream.c',
  'gstpipewirecamera.c',
  'gstpipewiredeviceprovider.c',
]

gstpipewire = shared_library('gstpipewire',
    gstpipewire_sources,
    include_directories : [ configinc ],
    dependencies : [ 
        gst_dep, 
        gst_base_dep, 
        gst_video_dep, 
        gst_audio_dep, 
        gst_allocators_dep,
        droidmedia_dep,
        libyuv_dep,
        threads_dep,
        mathlib 
    ],
    install : true,
    install_dir : '@0@/gstreamer-1.0'.format(get_option('libdir')),
)

plugins = [gstpipewire]
