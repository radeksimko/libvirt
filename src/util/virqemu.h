/*
 * virqemu.h: utilities for working with qemu and its tools
 *
 * Copyright (C) 2009, 2012-2016 Red Hat, Inc.
 * Copyright (C) 2009 Daniel P. Berrange
 *
 * This library is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * This library is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with this library.  If not, see
 * <http://www.gnu.org/licenses/>.
 *
 */

#pragma once

#include "internal.h"
#include "virbuffer.h"
#include "virjson.h"

typedef int (*virQEMUBuildCommandLineJSONArrayFormatFunc)(const char *key,
                                                          virJSONValuePtr array,
                                                          virBufferPtr buf,
                                                          const char *skipKey);
int virQEMUBuildCommandLineJSONArrayBitmap(const char *key,
                                           virJSONValuePtr array,
                                           virBufferPtr buf,
                                           const char *skipKey);
int virQEMUBuildCommandLineJSONArrayNumbered(const char *key,
                                             virJSONValuePtr array,
                                             virBufferPtr buf,
                                             const char *skipKey);

int virQEMUBuildCommandLineJSON(virJSONValuePtr value,
                                virBufferPtr buf,
                                const char *skipKey,
                                virQEMUBuildCommandLineJSONArrayFormatFunc array);

char *
virQEMUBuildNetdevCommandlineFromJSON(virJSONValuePtr props,
                                      bool rawjson);

char *virQEMUBuildDriveCommandlineFromJSON(virJSONValuePtr src);

void virQEMUBuildBufferEscapeComma(virBufferPtr buf, const char *str);
