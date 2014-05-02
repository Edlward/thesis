#include "gpwrap.h"
#include <gphoto2/gphoto2.h>
#include <iostream>
#include <algorithm>
#include <memory>
#include <fstream>

namespace gp {
Exception::Exception(const std::string& msg, int gpnum) : 
	std::runtime_error("gphoto2 error " + msg
			+ ":" + std::to_string(gpnum)
			+ " (" + gp_result_as_string(gpnum) + ")")
{}

Context::Context() : context(gp_context_new()) {
	gp_context_set_error_func(context, error_func, NULL);
	gp_context_set_message_func(context, msg_func, NULL);
	gp_context_set_status_func(context, status_func, NULL);
	// debug logging is massive
#if 0
	// maybe should have a top-level class for this as it's not context specific
	// typecast because enum vs int in header
	gp_log_add_func(GP_LOG_DEBUG, (GPLogFunc)log_func, NULL);
#endif
}

Context::~Context() {
	gp_context_unref(context);
}

void Context::error_func(GPContext* /*context*/, const char *msg, void* /*data*/) {
	std::cerr << "!!! gphoto2 context error:\n"
		<< msg << std::endl;
}

void Context::msg_func(GPContext* /*context*/, const char* msg, void* /*data*/) {
	std::cerr << "!!! gphoto2 context message:\n"
		<< msg << std::endl;
}

void Context::status_func(GPContext* /*context*/, const char* msg, void* /*data*/) {
	std::cerr << "!!! gphoto2 context status:\n"
		<< msg << std::endl;
}

void Context::log_func(int level, const char* domain, const char* str, void* /*data*/) {
	std::cerr << "[gphoto2 log (level=" << level << ", domain=" << domain << "]: "
		<< str << std::endl;
}
Widget::~Widget() {
	// move ctor nulls this
	// HOX seems that parent deletes its children without looking at the refcount
	if (widget && !parent)
		gp_widget_unref(widget);

	if (parent)
		gp_widget_unref(parent->widget);

	if (camera)
		gp_camera_unref(camera->camera);
}

Widget::Widget(CameraWidget* widget, Camera& camera) :
	widget(widget), parent(nullptr), camera(&camera) {
	gp_camera_ref(camera.camera);
}

Widget::Widget(CameraWidget* widget, Widget& parent) :
	widget(widget), parent(&parent), camera(parent.camera) {
	gp_widget_ref(parent.widget);
	gp_camera_ref(camera->camera);
}

Widget::Widget(Widget&& other) :
	widget(other.widget), parent(other.parent), camera(other.camera) {
	other.widget = nullptr;
	other.parent = nullptr;
	other.camera = nullptr;
}

Widget Widget::operator[](const char* name_or_label) {
	CameraWidget* child;
	int ret = gp_widget_get_child_by_name(widget, name_or_label, &child);

	if (ret < GP_OK)
		ret = gp_widget_get_child_by_label(widget, name_or_label, &child);

	if (ret < GP_OK)
		throw std::out_of_range("no widget found");
	
	return Widget(child, *this);
}

Widget::WidgetType Widget::type() {
	CameraWidgetType type;
	gp_widget_get_type(widget, &type);
	switch (type) {
		case GP_WIDGET_WINDOW:  return WIDGET_WINDOW;
		case GP_WIDGET_SECTION: return WIDGET_SECTION;
		case GP_WIDGET_TEXT:    return WIDGET_TEXT;
		case GP_WIDGET_RANGE:   return WIDGET_RANGE;
		case GP_WIDGET_TOGGLE:  return WIDGET_TOGGLE;
		case GP_WIDGET_RADIO:   return WIDGET_RADIO;
		case GP_WIDGET_MENU:    return WIDGET_MENU;
		case GP_WIDGET_DATE:    return WIDGET_DATE;
		case GP_WIDGET_BUTTON:  return WIDGET_BUTTON;
	};
	throw std::invalid_argument("Unknown type " + std::to_string((int)type));
}

std::string Widget::Traits<std::string>::read(Widget& widget) {
	const char *val;
	gp_widget_get_value(widget.widget, &val);
	return val;
}

void Widget::Traits<std::string>::write(Widget& widget, const std::string& value) {
	gp_widget_set_value(widget.widget, const_cast<char*>(value.c_str()));
}

bool Widget::Traits<bool>::read(Widget& widget) {
	int val;
	gp_widget_get_value(widget.widget, &val);
	return static_cast<bool>(val);
}

void Widget::Traits<bool>::write(Widget& widget, bool value) {
	int val = (int)value;
	gp_widget_set_value(widget.widget, &val);
}

// bubble up the config to the root
// (todo: store root, not parent?)
void Widget::set_changed() {
	if (parent)
		parent->set_changed();
	else
		camera->set_config(*this);
}

void Camera::set_config(const Widget& cfg) {
	std::cout << "SET CFG" << std::endl;
	gp_camera_set_config(camera, cfg.widget, ctx.context);
}

Camera Context::auto_camera() {
	::Camera* cam;
	int ret;
	if ((ret = gp_camera_new(&cam)) < GP_OK)
		throw Exception("gp_camera_new", ret);

	if ((ret = gp_camera_init(cam, context)) < GP_OK) {
		gp_camera_unref(cam);
		throw Exception("gp_camera_init", ret);
	}
	return Camera(cam, *this);
}

std::vector<Camera> Context::all_cameras() {
	CameraList *list;
	int ret;
	if ((ret = gp_list_new(&list)) < GP_OK) {
		throw std::bad_alloc(); // or Exception
	}
	if ((ret = gp_camera_autodetect(list, context)) < GP_OK) {
		gp_list_free(list);
		throw Exception("gp_camera_autodetect", ret);
	}

	std::vector<Camera> cams;
	for (int i = 0, count = ret; i < count; i++) {
		const char *name, *value;
		gp_list_get_name(list, i, &name);
		gp_list_get_value(list, i, &value);
		std::cout << name << "|" << value << std::endl;
		// cannot emplace because private ctor
		cams.push_back(Camera(name, value, *this));
	}
	return cams;
}

Camera::Camera(::Camera* camera, Context& ctx) : camera(camera), ctx(ctx) {
	gp_context_ref(ctx.context);
}

// stolen from libphoto2 examples/autodetect.c
// most of this seems to be in gp_camera_init autodetection too
Camera::Camera(const char *model, const char *port, Context& ctx) : camera(nullptr), ctx(ctx) {
	// safe deleter to free camera memory
	auto cam_unref_ignore_error = [](::Camera* c) { gp_camera_unref(c); };
	typedef std::unique_ptr<::Camera, decltype(cam_unref_ignore_error)> SafeCamera;
#define GP_OR_THROW(ret, func, ...) \
	if ((ret = func(__VA_ARGS__)) < GP_OK) throw Exception(#func, ret);

	static GPPortInfoList		*portinfolist = nullptr;
	static CameraAbilitiesList	*abilities = nullptr;
	int		ret;

	//if ((ret = gp_camera_new (camera)) < GP_OK)
	//	throw Exception("gp_camera_new", ret);
	GP_OR_THROW(ret, gp_camera_new, &camera);
	SafeCamera cam_deleter(camera, cam_unref_ignore_error);

	if (!abilities) {
		/* Load all the camera drivers we have... */
		GP_OR_THROW(ret, gp_abilities_list_new, &abilities);
		GP_OR_THROW(ret, gp_abilities_list_load, abilities, ctx.context);
	}

	/* First lookup the model / driver */
	int model_idx;
    GP_OR_THROW(model_idx, gp_abilities_list_lookup_model, abilities, model);
	CameraAbilities my_abilities;
	GP_OR_THROW(ret, gp_abilities_list_get_abilities, abilities, model_idx, &my_abilities);
	GP_OR_THROW(ret, gp_camera_set_abilities, camera, my_abilities);

	if (!portinfolist) {
		/* Load all the port drivers we have... */
		GP_OR_THROW(ret, gp_port_info_list_new, &portinfolist);
		GP_OR_THROW(ret, gp_port_info_list_load, portinfolist);
	}

	/* Then associate the camera with the specified port */
	int path_idx;
	GP_OR_THROW(path_idx, gp_port_info_list_lookup_path, portinfolist, port);
	GPPortInfo myportinfo;
	GP_OR_THROW(ret, gp_port_info_list_get_info, portinfolist, path_idx, &myportinfo);
	GP_OR_THROW(ret, gp_camera_set_port_info, camera, myportinfo);

	cam_deleter.release(); // nothing can fail anymore here
	gp_context_ref(ctx.context);
}


Camera::Camera(Camera&& other) : camera(other.camera), ctx(other.ctx) {
	gp_context_ref(ctx.context);
	other.camera = nullptr;
}

Camera::~Camera() {
	gp_context_unref(ctx.context);
	//gp_camera_exit(camera,ctx.context);
	if (camera)
		gp_camera_unref(camera); // freeing also exits it, i guess
}

Widget Camera::config() {
	CameraWidget *w;
	int ret;
	if ((ret = gp_camera_get_config(camera, &w, ctx.context)) < GP_OK)
		throw Exception("gp_camera_get_config", ret);
	return Widget(w, *this);
}

std::vector<char> Camera::preview() {
	CameraFile* file;
	int ret;
	if ((ret = gp_file_new(&file)) < GP_OK)
		throw Exception("gp_file_new", ret);

	if ((ret = gp_camera_capture_preview(camera, file, ctx.context)) < GP_OK) {
		gp_file_unref(file);
		throw Exception("gp_camera_capture_preview", ret);
	}

	const char *rawbuf;
	size_t nbytes;
	if ((ret = gp_file_get_data_and_size(file, &rawbuf, &nbytes)) < GP_OK) {
		gp_file_unref(file);
		throw Exception("gp_file_get_data_and_size", ret);
	}

	std::vector<char> buf(nbytes);
	std::copy(rawbuf, rawbuf + nbytes, &buf[0]);

	gp_file_unref(file);
	return buf;
}

void Camera::save_preview(const std::string& fname) {
	auto pic = preview();
	std::ofstream fs(fname);
	std::copy(pic.begin(), pic.end(), std::ostreambuf_iterator<char>(fs));
}

}
