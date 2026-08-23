/*
Sports Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-camera-list.h"

#include "sr-capture.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <stdlib.h>
#include <string.h>

struct camera_list_builder {
	char **names;
	size_t count;
	size_t capacity;
	bool failed;
};

static bool contains_name(const struct camera_list_builder *builder, const char *name)
{
	for (size_t i = 0; i < builder->count; i++) {
		if (strcmp(builder->names[i], name) == 0)
			return true;
	}
	return false;
}

static void append_name(struct camera_list_builder *builder, const char *name)
{
	if (builder->failed || !name || !*name || contains_name(builder, name))
		return;

	if (builder->count == builder->capacity) {
		const size_t next_capacity = builder->capacity ? builder->capacity * 2 : 8;
		char **next = brealloc(builder->names, next_capacity * sizeof(*next));
		if (!next) {
			builder->failed = true;
			return;
		}
		builder->names = next;
		builder->capacity = next_capacity;
	}

	char *copy = bstrdup(name);
	if (!copy) {
		builder->failed = true;
		return;
	}
	builder->names[builder->count++] = copy;
}

static void enum_capture_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct camera_list_builder *builder = param;
	if (!builder || builder->failed || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;
	append_name(builder, obs_source_get_name(parent));
}

static bool enum_source(void *param, obs_source_t *source)
{
	struct camera_list_builder *builder = param;
	if (!builder || builder->failed)
		return false;
	obs_source_enum_filters(source, enum_capture_filter, builder);
	return !builder->failed;
}

static int compare_names(const void *a, const void *b)
{
	const char *const *lhs = a;
	const char *const *rhs = b;
	return strcmp(*lhs, *rhs);
}

bool sr_camera_list_capture(struct sr_camera_list *list)
{
	if (!list)
		return false;
	memset(list, 0, sizeof(*list));

	struct camera_list_builder builder = {0};
	obs_enum_sources(enum_source, &builder);
	if (builder.failed) {
		for (size_t i = 0; i < builder.count; i++)
			bfree(builder.names[i]);
		bfree(builder.names);
		return false;
	}

	if (builder.count > 1)
		qsort(builder.names, builder.count, sizeof(*builder.names), compare_names);
	list->names = builder.names;
	list->count = builder.count;
	return true;
}

void sr_camera_list_free(struct sr_camera_list *list)
{
	if (!list)
		return;
	for (size_t i = 0; i < list->count; i++)
		bfree(list->names[i]);
	bfree(list->names);
	memset(list, 0, sizeof(*list));
}
