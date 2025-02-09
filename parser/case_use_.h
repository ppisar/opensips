/*
 * Copyright (C) 2022 OpenSIPS Solutions
 *
 * This file is part of opensips, a free SIP server.
 *
 * opensips is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version
 *
 * opensips is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, write to the Free Software
 * Foundation, Inc., 51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA
 */

#ifndef CASE_USE__H
#define CASE_USE__H

#define path_CASE2                        \
	if (LOWER_DWORD(val) == _path_) {     \
		hdr->type = HDR_USE_PATH_T;       \
		hdr->name.len = 8;                \
		p += 4;                           \
		goto dc_cont;                     \
	}


#define use__CASE          \
		p += 4;            \
		val = READ(p);     \
		path_CASE2;        \
		goto other;

#endif /* CASE_USE__H */
