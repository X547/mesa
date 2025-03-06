# Copyright © 2021 Ilia Mirkin
# SPDX-License-Identifier: MIT

inc_nouveau_winsys=include_directories('winsys')

#subdir('drm')
subdir('headers')
#subdir('winsys')
if with_nouveau_vk
  subdir('rust')
  subdir('nil')
  subdir('compiler')
endif
if with_tools.contains('drm-shim')
  subdir('drm-shim')
endif

if with_nouveau_vk
  subdir('mme')
  subdir('vulkan')
endif
