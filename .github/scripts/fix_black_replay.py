from pathlib import Path

event = Path("src/sr-event-output.c")
s = event.read_text()
old = ".output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_CUSTOM_DRAW | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,"
new = ".output_flags = OBS_SOURCE_VIDEO | OBS_SOURCE_AUDIO | OBS_SOURCE_DO_NOT_DUPLICATE,"
assert old in s, "Event Output flag line changed unexpectedly"
s = s.replace(old, new, 1)
event.write_text(s)

capture = Path("src/capture-filter.c")
s = capture.read_text()

old = "struct sr_capture {\n\tobs_source_t *self;\n\tstruct sr_buffer buffer;"
new = "struct sr_capture {\n\tobs_source_t *self;\n\tpthread_mutex_t parent_mutex;\n\tobs_weak_source_t *parent_weak;\n\tchar camera_name[256];\n\tstruct sr_buffer buffer;"
assert old in s, "capture struct anchor changed unexpectedly"
s = s.replace(old, new, 1)

old = "static void sr_capture_gpu_render(void *data, uint32_t cx, uint32_t cy);\n\n"
new = '''static void sr_capture_gpu_render(void *data, uint32_t cx, uint32_t cy);

static void sr_capture_set_parent(struct sr_capture *c, obs_source_t *parent)
{
	if (!c)
		return;

	obs_weak_source_t *weak = parent ? obs_source_get_weak_source(parent) : NULL;
	pthread_mutex_lock(&c->parent_mutex);
	obs_weak_source_t *old = c->parent_weak;
	c->parent_weak = weak;
	if (parent) {
		const char *name = obs_source_get_name(parent);
		if (name) {
			strncpy(c->camera_name, name, sizeof(c->camera_name) - 1);
			c->camera_name[sizeof(c->camera_name) - 1] = '\\0';
		}
	}
	pthread_mutex_unlock(&c->parent_mutex);

	if (old)
		obs_weak_source_release(old);
}

static obs_source_t *sr_capture_parent_ref(struct sr_capture *c)
{
	if (!c)
		return NULL;

	pthread_mutex_lock(&c->parent_mutex);
	obs_source_t *parent = c->parent_weak ? obs_weak_source_get_source(c->parent_weak) : NULL;
	pthread_mutex_unlock(&c->parent_mutex);
	return parent;
}

static void sr_capture_filter_add(void *data, obs_source_t *source)
{
	sr_capture_set_parent(data, source);
}

static void sr_capture_filter_remove(void *data, obs_source_t *source)
{
	UNUSED_PARAMETER(source);
	sr_capture_set_parent(data, NULL);
}

static void sr_capture_ensure_parent_from_filter_callback(struct sr_capture *c)
{
	if (!c)
		return;

	pthread_mutex_lock(&c->parent_mutex);
	const bool have_parent = c->parent_weak != NULL;
	pthread_mutex_unlock(&c->parent_mutex);
	if (have_parent)
		return;

	/* obs_filter_get_parent() is guaranteed by libobs inside filter_video.
	 * Cache only a weak reference here so the capture filter cannot keep its
	 * parent alive through a source/filter reference cycle. */
	obs_source_t *parent = obs_filter_get_parent(c->self);
	if (parent)
		sr_capture_set_parent(c, parent);
}

'''
assert old in s, "GPU render declaration anchor changed unexpectedly"
s = s.replace(old, new, 1)

old = "\tc->self = source;\n\tpthread_mutex_init(&c->status_mutex, NULL);"
new = "\tc->self = source;\n\tpthread_mutex_init(&c->parent_mutex, NULL);\n\tpthread_mutex_init(&c->status_mutex, NULL);"
assert old in s, "capture create anchor changed unexpectedly"
s = s.replace(old, new, 1)

old = "\tobs_remove_main_render_callback(sr_capture_gpu_render, c);\n\tset_parent_showing_hold(c, false);\n\n\tpthread_mutex_lock(&c->encode_mutex);"
new = "\tobs_remove_main_render_callback(sr_capture_gpu_render, c);\n\tset_parent_showing_hold(c, false);\n\tsr_capture_set_parent(c, NULL);\n\n\tpthread_mutex_lock(&c->encode_mutex);"
assert old in s, "capture destroy anchor changed unexpectedly"
s = s.replace(old, new, 1)

old = "\tpthread_mutex_destroy(&c->status_mutex);\n\tpthread_mutex_destroy(&c->camera_audio_mutex);"
new = "\tpthread_mutex_destroy(&c->status_mutex);\n\tpthread_mutex_destroy(&c->parent_mutex);\n\tpthread_mutex_destroy(&c->camera_audio_mutex);"
assert old in s, "capture mutex destroy anchor changed unexpectedly"
s = s.replace(old, new, 1)

old = '''static obs_source_t *capture_camera_source(struct sr_capture *c)
{
	return c ? obs_filter_get_parent(c->self) : NULL;
}

static const char *capture_camera_name(struct sr_capture *c)
{
	obs_source_t *parent = capture_camera_source(c);
	return parent ? obs_source_get_name(parent) : obs_source_get_name(c->self);
}'''
new = '''static obs_source_t *capture_camera_source_ref(struct sr_capture *c)
{
	return sr_capture_parent_ref(c);
}

static const char *capture_camera_name(struct sr_capture *c)
{
	return c && c->camera_name[0] ? c->camera_name : obs_source_get_name(c->self);
}'''
assert old in s, "camera source helper changed unexpectedly"
s = s.replace(old, new, 1)

old = '''\tobs_source_t *camera_source = capture_camera_source(c);
\tchar camera_key[SR_CAMERA_STABLE_KEY_MAX] = {0};
\tif (!camera_source || !sr_camera_key_from_source(camera_source, camera_key, sizeof(camera_key))) {
\t\tobs_log(LOG_ERROR, "'%s': could not resolve persistent OBS UUID for replay camera '%s'",
\t\t\tobs_source_get_name(c->self), capture_camera_name(c));
\t\tbfree(session_dir);
\t\tc->writer_failed = true;
\t\treturn false;
\t}'''
new = '''\tobs_source_t *camera_source = capture_camera_source_ref(c);
\tchar camera_key[SR_CAMERA_STABLE_KEY_MAX] = {0};
\tif (!camera_source || !sr_camera_key_from_source(camera_source, camera_key, sizeof(camera_key))) {
\t\tobs_log(LOG_ERROR, "'%s': could not resolve persistent OBS UUID for replay camera '%s'",
\t\t\tobs_source_get_name(c->self), capture_camera_name(c));
\t\tif (camera_source)
\t\t\tobs_source_release(camera_source);
\t\tbfree(session_dir);
\t\tc->writer_failed = true;
\t\treturn false;
\t}
\tobs_source_release(camera_source);'''
assert old in s, "ensure_writer camera source block changed unexpectedly"
s = s.replace(old, new, 1)

old = '''\tobs_source_t *target = obs_filter_get_target(c->self);
\tAVPacket *pkt = NULL;
\tconst uint64_t encode_start = os_gettime_ns();
\tconst bool encode_ok = sr_gpu_encoder_render_encode(c->gpu_encoder, target, &pkt);'''
new = '''\t/* The main render callback is outside libobs filter callbacks, where
\t * obs_filter_get_target()/get_parent() are not guaranteed valid. Resolve
\t * the weak parent cached by filter_add/filter_video into a strong ref for
\t * the duration of this render instead. */
\tobs_source_t *target = capture_camera_source_ref(c);
\tif (!target) {
\t\tpublish_status(c, obs_get_video_frame_time(), false);
\t\tpthread_mutex_unlock(&c->encode_mutex);
\t\treturn;
\t}
\tAVPacket *pkt = NULL;
\tconst uint64_t encode_start = os_gettime_ns();
\tconst bool encode_ok = sr_gpu_encoder_render_encode(c->gpu_encoder, target, &pkt);
\tobs_source_release(target);'''
assert old in s, "GPU target block changed unexpectedly"
s = s.replace(old, new, 1)

old = '''\tstruct sr_capture *c = data;
\tif (!c)
\t\treturn frame;

\tpthread_mutex_lock(&c->encode_mutex);'''
new = '''\tstruct sr_capture *c = data;
\tif (!c)
\t\treturn frame;

\tsr_capture_ensure_parent_from_filter_callback(c);
\tpthread_mutex_lock(&c->encode_mutex);'''
assert old in s, "filter_video entry anchor changed unexpectedly"
s = s.replace(old, new, 1)

old = '''\t.get_properties = sr_capture_properties,
\t.filter_video = sr_capture_filter_video,
\t.filter_audio = sr_capture_filter_audio,'''
new = '''\t.get_properties = sr_capture_properties,
\t.filter_add = sr_capture_filter_add,
\t.filter_remove = sr_capture_filter_remove,
\t.filter_video = sr_capture_filter_video,
\t.filter_audio = sr_capture_filter_audio,'''
assert old in s, "source info anchor changed unexpectedly"
s = s.replace(old, new, 1)

capture.write_text(s)
