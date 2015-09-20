#include "context.h"

#include <set>
#include <map>
#include <thread>
#include <mutex>
#include <vector>
#include <functional>

namespace gl {

/*
// Lookup table for BufferIndex to GL_TARGET_BUFFER enums.
// Obviously, BufferIndex order must match this table order.
// Have you updated either table recently? Do the orders 
// still match? Well, do they? Did you double check...?!
static const GLenum buffer_target[BUFFER_INDEX_MAX] {
  GL_ARRAY_BUFFER,
  // GL_ATOMIC_COUNTER_BUFFER,
  GL_COPY_READ_BUFFER,
  GL_COPY_WRITE_BUFFER,
  // GL_DISPATCH_INDIRECT_BUFFER,
  GL_DRAW_INDIRECT_BUFFER,
  GL_ELEMENT_ARRAY_BUFFER,
  GL_PIXEL_PACK_BUFFER,
  GL_PIXEL_UNPACK_BUFFER,
  // GL_QUERY_BUFFER,
  // GL_SHADER_STORAGE_BUFFER,
  GL_TEXTURE_BUFFER,
  GL_TRANSFORM_FEEDBACK_BUFFER,
  GL_UNIFORM_BUFFER,
};
*/

// My policy with the impl so far is only to forward function calls where I actually 
// need polymorphism. Mostly, I'll use impl class as an opaque data storage type.
// And I'm using pimpl at all, rather than pure virtual interface inheritance,
// because:
//   -  I want users to be able to put the concrete types on their stacks
//   -  I don't want to write factories all over the place
//   -  There's no intention to have multiple implementations of most things
//       (I mean, it's a wrapper...)
//   -  I'm trying to avoid future ABI compatibility issues
//   -  It's faster  :trollface:

class Context_impl {
  public:
    Context_impl(Context& context, void * handle);
    virtual ~Context_impl() =0;


  public:
    virtual void make_current() {}
    virtual bool current() const { return false; }
    void on_made_not_current();

  public:
    GLbitfield _clear_mask { GL_COLOR_BUFFER_BIT };

  protected:
    Context& _context;
    void *_handle { nullptr };
};


class MonoContext_impl : public Context_impl {
  private:
    static MonoContext_impl* current_context;

  public:
    MonoContext_impl(Context& context, void *);
    ~MonoContext_impl();

  public:
    void make_current() override;
    bool current() const override;

};


class MultiContext_impl : public Context_impl {
  private:
    static std::map<std::thread::id, MultiContext_impl*> current_context;
    static std::recursive_mutex current_context_lock;

  public:
    MultiContext_impl(MultiContext& context, void *);
    ~MultiContext_impl();

  public:
    void make_current() override;
    bool current() const override;

  private:
    std::thread::id _thread_id;

};



MonoContext_impl* MonoContext_impl::current_context { 0 };

std::map<std::thread::id, MultiContext_impl*> MultiContext_impl::current_context;
std::recursive_mutex MultiContext_impl::current_context_lock;




template<typename T, void(*GET)(GLenum, T*)>
inline void get(Context const& context, GLenum param, T* out) {
  if (!context.current()) {
    throw gl::exception("can't get param from inactive context");
  }
  GL_CALL(GET(param, out));
}

template<typename T, void(*GET)(GLenum, T*)>
inline T get(Context const& context, GLenum param) { // get single
  T rv;
  get<T, GET>(context, param, &rv);
  return rv;
}

template<typename T>
inline T get(Context const& context, GLenum param);

template<>
inline int get<int>(Context const& context, GLenum param) {
  return get<GLint, glGetIntegerv>(context, param);
}

template<>
inline int64_t get<int64_t>(Context const& context, GLenum param) {
  return get<GLint64, glGetInteger64v>(context, param);
}

template<>
inline bool get<bool>(Context const& context, GLenum param) {
  return get<GLboolean, glGetBooleanv>(context, param);
}

template<>
inline double get<double>(Context const& context, GLenum param) {
  return get<GLdouble, glGetDoublev>(context, param);
}

template<>
inline float get<float>(Context const& context, GLenum param) {
  return get<GLfloat, glGetFloatv>(context, param);
}

template<>
inline color get<color>(Context const& context, GLenum param) {
  color c;
  get<GLfloat, glGetFloatv>(context, param, reinterpret_cast<GLfloat*>(&c));
  return c;
}


unsigned Context::major_version() const {
  return (unsigned)gl::get<int>(*this, GL_MAJOR_VERSION);
}

unsigned Context::minor_version() const {
  return (unsigned)gl::get<int>(*this, GL_MINOR_VERSION);
}

Context::~Context() {
  delete _impl;
}

void Context_impl::on_made_not_current() {
}


void Context::clear() {
  GL_CALL(glClear(_impl->_clear_mask));
}

void Context::clear(GLbitfield mask) {
  GL_CALL(glClear(mask));
}

void Context::clear_color(color const& color) {
  GL_CALL(glClearColor(color.r, color.g, color.b, color.a));
}


Context::Context() {}

Context_impl::Context_impl(Context& context, void* handle): _context(context), _handle(handle) {}

Context_impl::~Context_impl() {
}

MonoContext_impl::MonoContext_impl(Context& context, void* handle): Context_impl(context, handle) {
  make_current();
}


MonoContext::MonoContext(void* handle): Context() {
  _impl = new MonoContext_impl(*this, handle);
}


void MonoContext_impl::make_current() {
  if (current_context && current_context != this) {
    current_context->on_made_not_current();
  }
  current_context = this;
}

bool MonoContext_impl::current() const {
  return current_context == this;
}


MonoContext_impl::~MonoContext_impl() {
  if (current_context == this) {
    current_context = nullptr;
  }
}


MultiContext_impl::MultiContext_impl(MultiContext& context, void* handle)
  : Context_impl(context, handle)
  , _thread_id(std::this_thread::get_id())
{
  make_current();
}

void MultiContext_impl::make_current() {
  if (std::this_thread::get_id() != _thread_id) {
    throw gl::exception("attempt to make Context current on another thread");
  }
  {
    std::lock_guard<std::recursive_mutex> lock(current_context_lock);
    auto& i = current_context[_thread_id]; 
    if (i != this) {
      if (i) {
        i->on_made_not_current();
      }
      i = this;
    }
  }
  Context_impl::make_current();
}

bool MultiContext_impl::current() const {
  if (std::this_thread::get_id() != _thread_id) {
    return false;
  }
  std::lock_guard<std::recursive_mutex> lock(current_context_lock);
  auto it = current_context.find(_thread_id);
  return it != current_context.end() && it->second == this;
}

MultiContext_impl::~MultiContext_impl() {
  std::lock_guard<std::recursive_mutex> lock(current_context_lock);
  auto it = current_context.find(_thread_id);
  if (it != current_context.end() && it->second == this) {
    current_context.erase(it);
  }
}


bool Context::current() const {
  return _impl->current();
}

void Context::make_current() {
  _impl->make_current();
}


MonoContext::~MonoContext() {}



template<GLenum capability>
void Context::enable() {
  GL_CALL(glEnable(capability));
}

template<GLenum capability>
void Context::disable() {
  GL_CALL(glDisable(capability));
}

template<GLenum capability>
bool Context::is_enabled() const {
  auto rv = GL_CALL(glIsEnabled(capability));
  return rv;
}

template<GLenum P, typename T>
T Context::get() const {
  return gl::get<T>(*this, P);
}


#define INSTANTIATE_ENABLE(MODE) \
  template void Context::enable< MODE >(); \
  template void Context::disable< MODE >(); \
  template bool Context::is_enabled<MODE>() const;

INSTANTIATE_ENABLE(GL_BLEND);
INSTANTIATE_ENABLE(GL_COLOR_LOGIC_OP);
INSTANTIATE_ENABLE(GL_CULL_FACE);
INSTANTIATE_ENABLE(GL_DEPTH_CLAMP);
INSTANTIATE_ENABLE(GL_DEPTH_TEST);
INSTANTIATE_ENABLE(GL_DITHER);
INSTANTIATE_ENABLE(GL_FRAMEBUFFER_SRGB);
INSTANTIATE_ENABLE(GL_LINE_SMOOTH);
INSTANTIATE_ENABLE(GL_MULTISAMPLE);
INSTANTIATE_ENABLE(GL_POLYGON_OFFSET_FILL);
INSTANTIATE_ENABLE(GL_POLYGON_OFFSET_LINE);
INSTANTIATE_ENABLE(GL_POLYGON_OFFSET_POINT);
INSTANTIATE_ENABLE(GL_POLYGON_SMOOTH);
INSTANTIATE_ENABLE(GL_PRIMITIVE_RESTART);
INSTANTIATE_ENABLE(GL_RASTERIZER_DISCARD);
INSTANTIATE_ENABLE(GL_SAMPLE_ALPHA_TO_COVERAGE);
INSTANTIATE_ENABLE(GL_SAMPLE_ALPHA_TO_ONE);
INSTANTIATE_ENABLE(GL_SAMPLE_COVERAGE);
INSTANTIATE_ENABLE(GL_SAMPLE_SHADING);
INSTANTIATE_ENABLE(GL_SAMPLE_MASK);
INSTANTIATE_ENABLE(GL_SCISSOR_TEST);
INSTANTIATE_ENABLE(GL_STENCIL_TEST);
INSTANTIATE_ENABLE(GL_TEXTURE_CUBE_MAP_SEAMLESS);
INSTANTIATE_ENABLE(GL_PROGRAM_POINT_SIZE);
#if GL_4_5
INSTANTIATE_ENABLE(GL_DEBUG_OUTPUT);
INSTANTIATE_ENABLE(GL_DEBUG_OUTPUT_SYNCHRONOUS);
INSTANTIATE_ENABLE(GL_PRIMITIVE_RESTART_FIXED_INDEX);
#endif

#undef INSTANTIATE_ENABLE



} // namespace gl
