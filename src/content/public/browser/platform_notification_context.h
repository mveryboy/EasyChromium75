// Copyright 2015 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CONTENT_PUBLIC_BROWSER_PLATFORM_NOTIFICATION_CONTEXT_H_
#define CONTENT_PUBLIC_BROWSER_PLATFORM_NOTIFICATION_CONTEXT_H_

#include <stdint.h>

#include <string>
#include <vector>

#include "base/callback.h"
#include "base/memory/ref_counted.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_database_data.h"

class GURL;

namespace blink {
struct NotificationResources;
}  // namespace blink

namespace content {

// Represents the storage context for persistent Web Notifications, specific to
// the storage partition owning the instance. All methods defined in this
// interface may only be used on the IO thread.
class PlatformNotificationContext
    : public base::RefCountedThreadSafe<PlatformNotificationContext,
                                        BrowserThread::DeleteOnUIThread> {
 public:
  using ReadResultCallback =
      base::OnceCallback<void(bool /* success */,
                              const NotificationDatabaseData&)>;

  using ReadResourcesResultCallback =
      base::OnceCallback<void(bool /* success */,
                              const blink::NotificationResources&)>;

  using ReadAllResultCallback =
      base::OnceCallback<void(bool /* success */,
                              const std::vector<NotificationDatabaseData>&)>;

  using WriteResultCallback =
      base::OnceCallback<void(bool /* success */,
                              const std::string& /* notification_id */)>;

  using DeleteResultCallback = base::OnceCallback<void(bool /* success */)>;

  using DeleteAllResultCallback =
      base::OnceCallback<void(bool /* success */, size_t /* deleted_count */)>;

  // Reasons for updating a notification, triggering a read.
  enum class Interaction {
    // No interaction was taken with the notification.
    NONE,

    // An action button in the notification was clicked.
    ACTION_BUTTON_CLICKED,

    // The notification itself was clicked.
    CLICKED,

    // The notification was closed.
    CLOSED
  };

  // Reads the data associated with |notification_id| belonging to |origin|
  // from the database. |callback| will be invoked with the success status
  // and a reference to the notification database data when completed.
  // |interaction| is passed in for UKM logging purposes and does not
  // otherwise affect the read.
  virtual void ReadNotificationDataAndRecordInteraction(
      const std::string& notification_id,
      const GURL& origin,
      Interaction interaction,
      ReadResultCallback callback) = 0;

  // Reads the resources associated with |notification_id| belonging to |origin|
  // from the database. |callback| will be invoked with the success status
  // and a reference to the notification resources when completed.
  virtual void ReadNotificationResources(
      const std::string& notification_id,
      const GURL& origin,
      ReadResourcesResultCallback callback) = 0;

  // Reads all data associated with |service_worker_registration_id| belonging
  // to |origin| from the database. |callback| will be invoked with the success
  // status and a vector with all read notification data when completed.
  virtual void ReadAllNotificationDataForServiceWorkerRegistration(
      const GURL& origin,
      int64_t service_worker_registration_id,
      ReadAllResultCallback callback) = 0;

  // Writes the data associated with a notification to a database and displays
  // it either immediately or at the desired time if the notification has a show
  // trigger defined. When this action is completed, |callback| will be invoked
  // with the success status and the notification id when written successfully.
  // The notification ID field for |database_data| will be generated, and thus
  // must be empty.
  virtual void WriteNotificationData(
      int64_t persistent_notification_id,
      int64_t service_worker_registration_id,
      const GURL& origin,
      const NotificationDatabaseData& database_data,
      WriteResultCallback callback) = 0;

  // Deletes all data associated with |notification_id| belonging to |origin|
  // from the database. |callback| will be invoked with the success status
  // when the operation has completed.
  virtual void DeleteNotificationData(const std::string& notification_id,
                                      const GURL& origin,
                                      DeleteResultCallback callback) = 0;

  // Checks permissions for all notifications in the database and deletes all
  // that do not have the permission anymore.
  virtual void DeleteAllNotificationDataForBlockedOrigins(
      DeleteAllResultCallback callback) = 0;

  // Trigger all pending notifications.
  virtual void TriggerNotifications() = 0;

 protected:
  friend class base::DeleteHelper<PlatformNotificationContext>;
  friend struct BrowserThread::DeleteOnThread<BrowserThread::UI>;

  virtual ~PlatformNotificationContext() {}
};

}  // namespace content

#endif  // CONTENT_PUBLIC_BROWSER_PLATFORM_NOTIFICATION_CONTEXT_H_