#pragma once

#include <plainroot/toolkit/toolkit.h>

#include <functional>

namespace plainroot::filechooser::detail {

using TaskPoster = std::function<plainroot_toolkit_result_v1(
    plainroot_toolkit_application_task_fn callback, void *user_data,
    plainroot_toolkit_application_task_destroy_fn destroy)>;

inline plainroot_toolkit_result_v1 post_application_task(
    plainroot_toolkit_application *application, const TaskPoster &task_poster,
    plainroot_toolkit_application_task_fn callback, void *user_data,
    plainroot_toolkit_application_task_destroy_fn destroy) {
  if (task_poster)
    return task_poster(callback, user_data, destroy);
  return plainroot_toolkit_application_post_v1(application, callback,
                                               user_data, destroy);
}

} // namespace plainroot::filechooser::detail
