/*
Pitel Instant Replay
Copyright (C) 2026 Systec <systecinformatica@gmail.com> (https://www.systecinformatica.com.ar)

This program is free software; you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation; either version 2 of the License, or
(at your option) any later version.
*/

#include "sr-camera-list.h"

#include "sr-camera-identity.h"
#include "sr-capture.h"

#include <obs-module.h>
#include <util/bmem.h>

#include <stdlib.h>
#include <string.h>

struct camera_entry {
	char *name;
	char key[SR_CAMERA_STABLE_KEY_MAX];
};

struct camera_list_builder {
	struct camera_entry *items;
	size_t count;
	size_t capacity;
	bool failed;
};

static bool contains_key(const struct camera_list_builder *builder, const char *key)
{
	for (size_t i = 0; i < builder->count; i++) {
		if (strcmp(builder->items[i].key, key) == 0)
			return true;
	}
	return false;
}

static void append_camera(struct camera_list_builder *builder, obs_source_t *source)
{
	if (builder->failed || !source)
		return;

	char key[SR_CAMERA_STABLE_KEY_MAX] = {0};
	if (!sr_camera_key_from_source(source, key, sizeof(key))) {
		builder->failed = true;
		return;
	}
	if (contains_key(builder, key))
		return;

	const char *name = obs_source_get_name(source);
	if (!name || !*name) {
		builder->failed = true;
		return;
	}

	if (builder->count == builder->capacity) {
		const size_t next_capacity = builder->capacity ? builder->capacity * 2 : 8;
		struct camera_entry *next = brealloc(builder->items, next_capacity * sizeof(*next));
		if (!next) {
			builder->failed = true;
			return;
		}
		builder->items = next;
		builder->capacity = next_capacity;
	}

	struct camera_entry *entry = &builder->items[builder->count];
	memset(entry, 0, sizeof(*entry));
	entry->name = bstrdup(name);
	if (!entry->name) {
		builder->failed = true;
		return;
	}
	memcpy(entry->key, key, strlen(key) + 1);
	builder->count++;
}

static void enum_capture_filter(obs_source_t *parent, obs_source_t *child, void *param)
{
	struct camera_list_builder *builder = param;
	if (!builder || builder->failed || strcmp(obs_source_get_unversioned_id(child), SR_CAPTURE_ID) != 0)
		return;
	append_camera(builder, parent);
}

static bool enum_source(void *param, obs_source_t *source)
{
	struct camera_list_builder *builder = param;
	if (!builder || builder->failed)
		return false;
	obs_source_enum_filters(source, enum_capture_filter, builder);
	return !builder->failed;
}

static int compare_entries(const void *a, const void *b)
{
	const struct camera_entry *lhs = a;
	const struct camera_entry *rhs = b;
	const int key_order = strcmp(lhs->key, rhs->key);
	return key_order ? key_order : strcmp(lhs->name, rhs->name);
}

static void free_builder(struct camera_list_builder *builder)
{
	if (!builder)
		return;
	for (size_t i = 0; i < builder->count; i++)
		bfree(builder->items[i].name);
	bfree(builder->items);
	memset(builder, 0, sizeof(*builder));
}

bool sr_camera_list_capture(struct sr_camera_list *list)
{
	if (!list)
		return false;
	memset(list, 0, sizeof(*list));

	struct camera_list_builder builder = {0};
	obs_enum_sources(enum_source, &builder);
	if (builder.failed) {
		free_builder(&builder);
		return false;
	}

	if (builder.count > 1)
		qsort(builder.items, builder.count, sizeof(*builder.items), compare_entries);

	if (builder.count) {
		list->names = bmalloc(builder.count * sizeof(*list->names));
		if (!list->names) {
			free_builder(&builder);
			return false;
		}
		for (size_t i = 0; i < builder.count; i++) {
			list->names[i] = builder.items[i].name;
			builder.items[i].name = NULL;
		}
	}
	list->count = builder.count;
	free_builder(&builder);
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
