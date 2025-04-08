project('gstpipewire', 'c',
  version : '1.0.0',
  meson_version : '>= 0.50.0',
  default_options : ['warning_level=2', 'buildtype=debugoptimized']
)

cc = meson.get_compiler('c')

gst_req = '>= 1.20.0'
gst_dep = dependency('gstreamer-1.0', version : gst_req)
gst_base_dep = dependency('gstreamer-base-1.0', version : gst_req)
gst_app_dep = dependency('gstreamer-app-1.0', version : gst_req)
gst_video_dep = dependency('gstreamer-video-1.0', version : gst_req)
gst_audio_dep = dependency('gstreamer-audio-1.0', version : gst_req)
gst_allocators_dep = dependency('gstreamer-allocators-1.0', version : gst_req)

droidmedia_dep = dependency('droidmedia', required : true)
libyuv_dep = cc.find_library('yuv', required : true)
threads_dep = dependency('threads', required : true)

mathlib = cc.find_library('m', required : false)

configinc = include_directories('.')

subdir('src')
