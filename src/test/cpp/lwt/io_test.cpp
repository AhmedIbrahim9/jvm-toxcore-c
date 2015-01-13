#include "lwt/io.h"
#include "lwt/logging.h"

#include "lwt/optional.h"
#include "lwt/variant.h"
#include "lwt/types.h"

#include <boost/intrusive_ptr.hpp>

#include <algorithm>
#include <memory>
#include <vector>

#include <ev.h>

#include <fcntl.h>
#include <sys/stat.h>
#include <sys/types.h>


namespace lwt
{


template<typename T, typename ...Args>
static boost::intrusive_ptr<T>
make_ptr (Args &&...args)
{
  return boost::intrusive_ptr<T> (new T (std::forward<Args> (args)...));
}


/******************************************************************************
 * basic_io
 *****************************************************************************/

enum class io_state
{
  success,
  failure,
  waiting,
  blocked,
};

std::ostream &
operator << (std::ostream &os, io_state state)
{
  switch (state)
    {
    case io_state::success: return os << "success";
    case io_state::failure: return os << "failure";
    case io_state::waiting: return os << "waiting";
    case io_state::blocked: return os << "blocked";
    }
}

struct basic_io_base;

struct basic_io_state
{
  static std::size_t object_count;

  std::size_t const tag;
  std::size_t const id = object_count++;

  typedef boost::intrusive_ptr<basic_io_state> pointer;

  std::string type () const { return types::name (tag); }

  basic_io_state (basic_io_state const &) = delete;
  basic_io_state &operator = (basic_io_state const &) = delete;

  basic_io_state (std::size_t tag)
    : tag (tag)
    , refcount (0)
  { }

  virtual ~basic_io_state ()
  { }

  friend void intrusive_ptr_add_ref (basic_io_state *p)
  { ++p->refcount; }

  friend void intrusive_ptr_release (basic_io_state *p)
  { if (!--p->refcount) delete p; }

  virtual io_state state () const = 0;

  virtual void process (basic_io_base self, int fd) = 0;
  virtual pointer cancel () = 0;
  virtual void notify (basic_io_base self, basic_io_base success) = 0;

  struct cancelled { };

  std::size_t refcount;
};

std::size_t basic_io_state::object_count;


namespace states
{
  template<typename ...Success>
  struct success_t;

  template<typename Failure>
  struct failure_t;

  template<typename Failure, typename Callback,
           typename Success = typename std::result_of<Callback (int)>::type>
  struct waiting_t;

  template<typename IO, typename ...Callbacks>
  struct blocked_t;
}


struct basic_io_base
{
  static std::size_t object_count;

  explicit basic_io_base (basic_io_state::pointer io)
    : state_ (make_ptr<data> (io))
  {
    LOG_ASSERT (io != nullptr);
    LOG (INFO) << *this << "Creating io_base in state " << io->state ();
  }

  std::size_t id () const { return state_->io->id; }
  io_state state () const { return state_->io->state (); }

  void add_blocked_by (basic_io_base io)
  {
    assert (!is_blocked_by (io));
    state_->blocked.push_back (io);
  }

  bool is_blocked () const
  { return !state_->blocked.empty (); }

  void transition (basic_io_base new_state)
  {
    LOG_ASSERT (new_state != *this);

    LOG (INFO) << *this << "Transitioned from "
               << state () << " [" << id () << "] to " << new_state.state ()
               << " [" << new_state.id () << "]";
    state_->io = new_state.state_->io;

    LOG (INFO) << *this << "Moving "
               << new_state.state_->blocked.size ()
               << " blocked states to this";

    while (!new_state.state_->blocked.empty ())
      {
        state_->blocked.push_back (std::move (new_state.state_->blocked.back ()));
        new_state.state_->blocked.pop_back ();
      }
    LOG (INFO) << *this << "Now contains " << state_->blocked.size () << " blocked states";
  }


  void process (int fd)
  {
    LOG (INFO) << *this << "Processing event on " << fd;
    state_->io->process (*this, fd);
    notify ();
  }

  void cancel ()
  {
    LOG (INFO) << *this << "Cancelled";
    //transition (state_->io->cancel ());
    notify ();
    assert (false);
  }

  void notify ()
  {
    LOG_ASSERT (state () == io_state::success);

    std::size_t const count = state_->blocked.size ();
    while (!state_->blocked.empty ())
      {
        LOG (INFO) << *this << "Notifying "
                   << (count - state_->blocked.size () + 1) << "/" << count
                   << " blocked IOs";
        basic_io_base blocked = state_->blocked.back ();
        blocked.state_->io->notify (blocked, *this);

        state_->blocked.pop_back ();
      }
    if (count != 0)
      LOG (INFO) << *this << "Done notifying";
  }

  basic_io_state::pointer get () const
  {
    return state_->io;
  }

  friend std::ostream &operator << (std::ostream &os, basic_io_base const &io)
  { return os << "[" << io.state_->id << "/" << io.id () << "] "; }

  friend bool operator == (basic_io_base const &lhs, basic_io_base const &rhs)
  { return lhs.state_ == rhs.state_; }

  friend bool operator != (basic_io_base const &lhs, basic_io_base const &rhs)
  { return lhs.state_ != rhs.state_; }

protected:
  bool is_blocked_by (basic_io_base io)
  {
    return std::find (state_->blocked.begin (), state_->blocked.end (), io)
      == state_->blocked.end ();
  }

  struct data
  {
    explicit data (basic_io_state::pointer io)
      : io (io)
    { }

    ~data ()
    {
      LOG_ASSERT (blocked.empty ());
    }

    friend void intrusive_ptr_add_ref (data *p)
    { ++p->refcount; }

    friend void intrusive_ptr_release (data *p)
    { if (!--p->refcount) delete p; }

    std::size_t const id = object_count++;
    std::size_t refcount = 0;
    basic_io_state::pointer io;
    // Actions that are blocked by this IO.
    std::vector<basic_io_base> blocked;
  };

  boost::intrusive_ptr<data> state_;
};

std::size_t basic_io_base::object_count;


template<typename Failure, typename ...Success>
struct basic_io
  : basic_io_base
{
  friend void type_name (std::string &name, basic_io const &)
  {
    name += "basic_io<";
    type_name<Failure, Success...> (name);
    name += ">";
  }

  using success_type = states::success_t<Success...>;
  using failure_type = states::failure_t<Failure>;

  template<typename Callback>
  using waiting_type = states::waiting_t<Failure, Callback>;

  template<typename ...Callbacks>
  using blocked_type = states::blocked_t<basic_io, Callbacks...>;


  basic_io (boost::intrusive_ptr<success_type> p)
    : basic_io_base (p)
  { check (); }

  basic_io (boost::intrusive_ptr<failure_type> p)
    : basic_io_base (p)
  { check (); }

  template<typename Callback>
  basic_io (boost::intrusive_ptr<waiting_type<Callback>> p)
    : basic_io_base (p)
  { check (); }

  template<typename ...Callbacks>
  basic_io (boost::intrusive_ptr<blocked_type<Callbacks...>> p)
    : basic_io_base (p)
  { check (); }


  template<typename BindF>
  typename std::result_of<BindF (Success...)>::type
  operator ->* (BindF func);

private:
  static void check ()
  {
    static_assert (sizeof (basic_io) == sizeof (basic_io_base),
                   "No additional members must be defined in basic_io.");
  }
};


#if 0
template<typename Failure, typename Head, typename ...Tail>
basic_io<Failure, Head, Tail...>
aggregate (std::vector<basic_io<Failure, Head, Tail...>> const &results)
{
  LOG (FATAL) << "Unimplemented aggregation for " << results.size () << " "
              << types::name<basic_io<Failure, Head, Tail...>> ();
}


/**
 * Default aggregation for io<>.
 */
template<typename Failure>
basic_io<Failure>
aggregate (std::vector<basic_io<Failure>> const &results)
{
  for (basic_io<Failure> const &result : results)
    {
      LOG_ASSERT (result.state () == io_state::success);
      LOG_ASSERT (!result.is_blocked ());
    }
  return results.back ();
}
#endif



template<bool ...Values>
struct and_type;

template<bool B, bool ...Values>
struct and_type<B, Values...>
{
  static bool const value = B && and_type<Values...>::value;
};

template<>
struct and_type<>
  : std::true_type
{ };


template<typename ...Success>
struct states::success_t
  : basic_io_state
{
  friend void type_name (std::string &name, success_t const &)
  {
    name += "success_t<";
    type_name<Success...> (name);
    name += ">";
  }

  static std::size_t const TAG;

  io_state state () const final { return io_state::success; }

  static_assert (and_type<std::is_same<
                   typename std::remove_reference<Success>::type,
                   Success
                 >::value...>::value,
                 "No references allowed in success_t");

  explicit success_t (Success const &...values)
    : basic_io_state (TAG)
    , data_ (values...)
  {
    LOG (INFO) << "[_/" << this->id << "] New success_t";
  }

  ~success_t ()
  {
    LOG (INFO) << "[_/" << this->id << "] Deleting success_t";
  }

  void process (basic_io_base self, int fd) final
  {
    LOG (FATAL) << "[_/" << self.id () << "] Processing event in success value: " << fd;
  }

  pointer cancel  () final
  {
    LOG (FATAL) << "[_/" << this->id << "] Attempted to cancel a success value";
    return this;
  }

  void notify (basic_io_base self, basic_io_base success) final
  {
    LOG (FATAL) << "[_/" << self.id () << "] Notifying success value with [" << success.id () << "]";
  }

  template<typename BindF>
  typename std::result_of<BindF (Success...)>::type
  operator ->* (BindF func)
  {
    return apply (make_seq<sizeof... (Success)> (), func);
  }

private:
  template<std::size_t ...S, typename BindF>
  typename std::result_of<BindF (Success...)>::type
  apply (seq<S...>, BindF func)
  {
    return func (std::get<S> (data_)...);
  }

  std::tuple<Success...> data_;
};

template<typename ...Success>
std::size_t const states::success_t<Success...>::TAG
    = types::make<states::success_t<Success...>> ();


template<typename Failure>
struct states::failure_t
  : basic_io_state
{
  friend void type_name (std::string &name, failure_t const &)
  {
    name += "failure_t<";
    type_name<Failure> (name);
    name += ">";
  }

  static std::size_t const TAG;

  io_state state () const final { return io_state::failure; }

  explicit failure_t (Failure failure)
    : basic_io_state (TAG)
    , data_ (failure)
  {
    LOG (INFO) << "[_/" << this->id << "] New failure_t";
  }

  ~failure_t ()
  {
    LOG (INFO) << "[_/" << this->id << "] Deleting failure_t";
  }

  explicit failure_t (cancelled failure)
    : basic_io_state (TAG)
    , data_ (failure)
  { }

  void process (basic_io_base self, int fd) final
  {
    LOG (FATAL) << "[_/" << self.id () << "] Processing event in failure value: " << fd;
  }

  pointer cancel  () final
  {
    LOG (FATAL) << "[_/" << this->id << "] Attempted to cancel a failure value";
    return this;
  }

  void notify (basic_io_base self, basic_io_base success) final
  {
    LOG (FATAL) << "[_/" << self.id () << "] Notifying failure value with [" << success.id () << "]";
  }

private:
  variant<Failure, cancelled> data_;
};

template<typename Failure>
std::size_t const states::failure_t<Failure>::TAG
    = types::make<states::failure_t<Failure>> ();


struct basic_io_blocked_state
  : basic_io_state
{
  using basic_io_state::basic_io_state;

  io_state state () const final { return io_state::blocked; }
};


template<typename Failure, typename ...Success, typename ...Callbacks>
struct states::blocked_t<basic_io<Failure, Success...>, Callbacks...>
  : basic_io_blocked_state
{
  typedef basic_io<Failure, Success...> io_type;
  typedef std::tuple<Callbacks...> callbacks_type;

  friend void type_name (std::string &name, blocked_t const &)
  {
    name += "blocked_t<";
    type_name<io_type, Callbacks...> (name);
    name += ">";
  }

  static std::size_t const TAG;

  explicit blocked_t (Callbacks ...callbacks)
    : basic_io_blocked_state (TAG)
    , callbacks_ (callbacks...)
  {
    LOG (INFO) << "[_/" << this->id << "] New blocked_t";
  }

  ~blocked_t ()
  {
    LOG (INFO) << "[_/" << this->id << "] Deleting blocked_t";
  }

  void process (basic_io_base self, int fd) final
  {
    LOG (FATAL) << "[_/" << self.id () << "] Processing event in blocked state: " << fd;
  }

  pointer cancel  () final
  {
    LOG (FATAL) << "[_/" << this->id << "] Attempted to cancel a blocked state";
    return this;
  }

  void notify (basic_io_base self, basic_io_base success) final
  {
    LOG_ASSERT (this->id == self.id ());

    LOG (INFO) << "[_/" << this->id << "] blocked_t became unblocked by [" << success.id () << "]";
    switch (success.state ())
      {
      case io_state::success:
        LOG (INFO) << "[_/" << this->id << "] calling " << sizeof... (Callbacks) << " callback(s)";
        break;
      case io_state::waiting:
        LOG (INFO) << "[_/" << this->id << "] but it is waiting; adding self to the blocked list of [" << success.id () << "]";
        success.add_blocked_by (self);
        return;
      default:
        LOG (FATAL) << "Invalid state in blocked_t::notify: " << success.state ();
        break;
      }

    auto results = invoke_all<sizeof... (Callbacks), std::tuple<>>::invoke (success, callbacks_);

    LOG (INFO) << "[_/" << this->id << "] Aggregating results";

    io_type result = aggregate_results (make_seq<sizeof... (Callbacks)> (), results);
    LOG_ASSERT (!result.is_blocked ());

    self.transition (result);
    if (result.state () == io_state::success)
      self.notify ();
  }

private:
  template<std::size_t ...S, typename ...Results>
  static io_type
  aggregate_results (seq<S...>, std::tuple<Results...> const &results)
  {
    return aggregate_results (std::get<S> (results)...);
  }


  static io_type
  aggregate_results (io_type const &io)
  {
    return io;
  }

  template<typename R1, typename R2, typename ...Rest>
  static io_type
  aggregate_results (R1 const &r1, R2 const &r2, Rest const &...rest)
  {
    return aggregate_results (aggregate (r1, r2), rest...);
  }


  template<std::size_t N, typename Results>
  struct invoke_all;

  template<std::size_t N, typename ...Results>
  struct invoke_all<N, std::tuple<Results...>>
  {
    template<typename T, typename ...Args>
    static io_type invoke_one_helper (basic_io_base success, T func)
    {
      return type_cast<success_t<typename std::decay<Args>::type...> &> (*success.get ())
        ->* func;
    }

    template<typename T, typename ...Args>
    static io_type invoke_one (io_type (T::*) (Args...), basic_io_base success, T func)
    { return invoke_one_helper<T, Args...> (success, func); }

    template<typename T, typename ...Args>
    static io_type invoke_one (io_type (T::*) (Args...) const, basic_io_base success, T func)
    { return invoke_one_helper<T, Args...> (success, func); }


    static auto invoke (basic_io_base success,
                        callbacks_type const &callbacks,
                        Results &&...results)
    {
      auto &head = std::get<N - 1> (callbacks);
      typedef typename std::tuple_element<N - 1, callbacks_type>::type head_type;
      return invoke_all<N - 1, std::tuple<Results..., io_type>>::
        invoke (success, callbacks,
                std::move (results)...,
                invoke_one (&head_type::operator (), success, head));
    }
  };

  template<typename ...Results>
  struct invoke_all<0, std::tuple<Results...>>
  {
    static auto invoke (basic_io_base /*success*/,
                        callbacks_type const &/*callbacks*/,
                        Results &&...results)
    {
      static_assert (std::tuple_size<callbacks_type>::value == sizeof... (Results),
                     "Unexpected result tuple size");
      return std::tuple<Results...> (std::move (results)...);
    }
  };

  callbacks_type callbacks_;
};

template<typename Failure, typename ...Success, typename ...Callbacks>
std::size_t const states::blocked_t<basic_io<Failure, Success...>, Callbacks...>::TAG
    = types::make<states::blocked_t<basic_io<Failure, Success...>, Callbacks...>> ();


template<typename Failure, typename ...Success>
struct basic_io_waiting_state
  : basic_io_state
{
  friend void type_name (std::string &name, basic_io_waiting_state const &)
  {
    name += "waiting_t<";
    type_name<Failure, Success...> (name);
    name += ">";
  }

  static std::size_t const TAG;

  basic_io_waiting_state ()
    : basic_io_state (TAG)
  { }

  io_state state () const final { return io_state::waiting; }
};

template<typename Failure, typename ...Success>
std::size_t const basic_io_waiting_state<Failure, Success...>::TAG
    = types::make<basic_io_waiting_state<Failure, Success...>> ();


template<typename Failure, typename Callback, typename ...Success>
struct states::waiting_t<Failure, Callback, basic_io<Failure, Success...>>
  : basic_io_waiting_state<Failure, Success...>
{
  typedef typename waiting_t::pointer pointer;
  typedef typename waiting_t::cancelled cancelled;

  typedef typename std::result_of<Callback (int)>::type io_type;

  explicit waiting_t (Callback callback)
    : callback_ (callback)
  {
    LOG (INFO) << "[_/" << this->id << "] New waiting_t";
  }

  ~waiting_t ()
  {
    LOG (INFO) << "[_/" << this->id << "] Deleting waiting_t";
  }

  void process (basic_io_base self, int fd) final
  {
    LOG (INFO) << "[_/" << self.id () << "] waiting_t became unblocked for fd " << fd;
    auto io = callback_ (fd);
    self.transition (io);
  }

  pointer cancel  () final
  {
    return pointer (make_ptr<states::failure_t<Failure>> (cancelled ()));
  }

  void notify (basic_io_base self, basic_io_base success) final
  {
    LOG (FATAL) << "[_/" << self.id () << "] Attempted to notify waiting I/O with ["
                << success.id () << "]";
  }

private:
  Callback callback_;
};


template<typename Failure, typename ...Success>
template<typename BindF>
typename std::result_of<BindF (Success...)>::type
basic_io<Failure, Success...>::operator ->* (BindF func)
{
  typedef typename std::result_of<BindF (Success...)>::type result_type;

  switch (state ())
    {
    case io_state::success:
      LOG (INFO) << "[_/" << this->id () << "] State is success; immediately calling next function";
      return type_cast<states::success_t<Success...> &> (*state_->io)
        ->* func;
    case io_state::failure:
      LOG (INFO) << "[_/" << this->id () << "] State is failure; immediately propagating error";
      return result_type (&type_cast<states::failure_t<Failure> &> (*state_->io));
    case io_state::waiting:
    case io_state::blocked:
      {
        //auto &waiting = type_cast<basic_io_waiting_state<Failure, Success...> &> (*io);

        result_type blocked (make_ptr<typename result_type::template blocked_type<BindF>> (func));

        LOG (INFO) << "[_/" << this->id () << "] Adding blocked IO: [" << blocked.id () << "]";
        this->state_->blocked.push_back (blocked);

        return result_type (blocked);
      }
    }
}


template<typename ...Types>
boost::intrusive_ptr<states::success_t<Types...>>
success (Types ...values)
{
  return make_ptr<states::success_t<Types...>> (values...);
}


template<typename Error>
boost::intrusive_ptr<states::failure_t<Error>>
failure (Error error)
{
  return make_ptr<states::failure_t<Error>> (error);
}



#if 0
template<typename ...Blocking>
//states::blocked_t<Blocking...>
boost::intrusive_ptr<states::blocked_t<>>
combine (Blocking .../*blocking*/)
{
  //return states::blocked_t<Blocking...> (blocking...);
  return make_ptr<states::blocked_t<>> ();
}
#endif


template<typename Func, typename ...Args>
auto
deferred (Func func, Args ...args)
{
  return [=] () mutable { return func (std::move (args)...); };
}


/******************************************************************************
 * UNIX I/O
 *****************************************************************************/


struct SystemError
{
  friend void type_name (std::string &name, SystemError const &)
  { name += "SystemError"; }

  explicit SystemError (int error)
    : code (error)
  { }

  int const code;
};

template<typename ...Success>
using io = basic_io<SystemError, Success...>;

template<typename ...Success>
using io_success = states::success_t<Success...>;

using io_failure = states::failure_t<SystemError>;

template<typename Callback>
using io_waiting = states::waiting_t<SystemError, Callback>;

template<typename ...Callbacks>
using io_blocked = states::blocked_t<Callbacks...>;



/******************************************************************************
 * event_loop
 *****************************************************************************/


struct io_waiting_ref
{
  io_waiting_ref &operator = (io_waiting_ref const &) = delete;

  io_waiting_ref (io_waiting_ref &&rhs)
    : io_ (std::move (rhs.io_))
  {
    rhs.processed_ = true;
  }

  io_waiting_ref (int events, basic_io_base io)
    : events (events)
    , io_ (io)
  { }

  ~io_waiting_ref ()
  {
    if (!processed_)
      io_.cancel ();
  }

  void process (int fd)
  {
    LOG_ASSERT (!processed_);
    io_.process (fd);
    processed_ = true;
  }

  int events;

private:
  bool processed_ = false;
  basic_io_base io_;
};


struct event_loop
{
  struct data_type
  {
    struct ev_loop *const raw_loop = ev_loop_new (EVFLAG_AUTO);
    std::vector<ev_io> io_watchers;
    std::vector<optional<io_waiting_ref>> io_waiting;
  };

  event_loop ()
    : data (new data_type)
  {
    LOG (INFO) << "Creating event loop";
  }

  ~event_loop ()
  {
    ev_loop_destroy (data->raw_loop);
  }


  static void io_callback (struct ev_loop *loop, ev_io *w, int events)
  {
    (void) loop;
    data_type *data = static_cast<data_type *> (w->data);

    // This fd was never waited on, before.
    LOG_ASSERT (data->io_waiting.size () > static_cast<std::size_t> (w->fd));

    optional<io_waiting_ref> waiting = std::move (data->io_waiting[w->fd]);
    if (waiting && waiting->events & events)
      {
        assert (!data->io_waiting[w->fd]);
        LOG (INFO) << "Received I/O event on " << w->fd << " for " << events;
        waiting->process (w->fd);
      }
    //ev_io_stop (data->raw_loop, &data->io_watchers[w->fd]);
  }

  void add_io (int fd)
  {
    LOG (INFO) << "Adding I/O watcher for fd " << fd;

    if (data->io_watchers.size () <= static_cast<std::size_t> (fd))
      data->io_watchers.resize (fd + 1);
    ev_io &io = data->io_watchers[fd];
    io.data = data.get ();
    ev_set_cb (&io, io_callback);
    ev_io_set (&io, fd, EV_READ | EV_WRITE);
  }

  void remove_io (int fd)
  {
    LOG (INFO) << "Removing I/O watcher for fd " << fd;

    LOG_ASSERT (data->io_watchers.size () > static_cast<std::size_t> (fd));
    ev_io_stop (data->raw_loop, &data->io_watchers[fd]);

    // Remove waiting IOs, instantly setting it to an error state.
    LOG_ASSERT (data->io_waiting.size () > static_cast<std::size_t> (fd));
    data->io_waiting[fd] = nullopt_t ();
  }


  template<typename Callback>
  typename std::result_of<Callback (int)>::type
  wait_io (int fd, int events, Callback cb)
  {
    typedef typename std::result_of<Callback (int)>::type result_type;

    LOG_ASSERT (data->io_watchers.size () > static_cast<std::size_t> (fd));
    result_type io = make_ptr<io_waiting<Callback>> (cb);

    if (data->io_waiting.size () <= static_cast<std::size_t> (fd))
      data->io_waiting.resize (fd + 1);

    if (data->io_waiting[fd])
      LOG (FATAL) << "Attempted to wait on the same fd (" << fd << ") twice at the same time";

    data->io_waiting[fd].emplace (events, io);
    ev_io_start (data->raw_loop, &data->io_watchers[fd]);

    return io;
  }


  void run (io<> program)
  {
    (void) program;
    ev_run (data->raw_loop);
    switch (program.state ())
      {
      case io_state::success:
        LOG (INFO) << "Program terminated with success";
        break;
      case io_state::failure:
        LOG (INFO) << "Program terminated with failure";
        break;
      case io_state::waiting:
        LOG (FATAL) << "Program terminated in waiting state";
        break;
      case io_state::blocked:
        LOG (FATAL) << "Program terminated in blocked state";
        break;
      }
  }

private:
  // Not copyable, moveable or assignable.
  std::unique_ptr<data_type> const data;
};


/******************************************************************************
 * I/O functions
 *****************************************************************************/


static thread_local event_loop default_loop;


io<int>
open (char const *pathname)
{
  int fd = ::open (pathname, 0);
  if (fd == -1)
    return failure (SystemError (errno));

  default_loop.add_io (fd);

  return success (fd);
}


io<>
close (int fd)
{
  if (::close (fd) == -1)
    return failure (SystemError (errno));

  default_loop.remove_io (fd);

  return success ();
}


io<std::vector<uint8_t>>
read (int fd, std::size_t count,
      std::vector<uint8_t> &&buffer = std::vector<uint8_t> (),
      std::size_t offset = 0)
{
  LOG (INFO) << "Registering I/O wait for read() on fd " << fd;
  return default_loop.wait_io (fd, EV_READ,
    [count, offset, buffer = std::move (buffer)] (int fd) mutable
      -> io<std::vector<uint8_t>>
    {
      LOG (INFO) << "read() became unblocked; reading " << count << " bytes"
                 << " at offset " << offset;
      if (buffer.size () <= count + offset)
        buffer.resize (count + offset);
      int result = ::read (fd, buffer.data () + offset, count);
      if (result == -1)
        return failure (SystemError (errno));
      LOG_ASSERT (static_cast<std::size_t> (result) <= count);
      buffer.resize (result + offset);
      return success (buffer);
    });
}


}


#include "lwt/io.h"
#include "lwt/logging.h"
#include <gtest/gtest.h>


namespace lwt
{


TEST (IO, Read) {
  typedef std::vector<uint8_t> byte_vec;

  io<> program = open ("/dev/random")
    ->* [] (int fd) -> io<> {
      io<byte_vec> waiting_read = read (fd, 10);

      // First waiting operation.
      io<> one = waiting_read
        ->* [=] (byte_vec const &buffer1) -> io<> {
          LOG (INFO) << "got buffer 1: " << buffer1.size ();
          return success ();
        }

        ->* deferred (read, fd, 10, byte_vec (), 0)

        ->* [=] (byte_vec const &buffer2) -> io<> {
          LOG (INFO) << "got buffer 2: " << buffer2.size ();
          return close (fd);
        };


      // Second waiting operation.
      io<> two = waiting_read
        ->* [=] (byte_vec const &buffer1) -> io<> {
          LOG (INFO) << "got buffer 1 again: " << buffer1.size ();
          return success ();
        };

      //return combine (one, two);
      return one;
    };

  default_loop.run (program);
}


}
