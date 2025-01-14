// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SERVICES_SERVICE_MANAGER_PUBLIC_CPP_TEST_TEST_SERVICE_MANAGER_H_
#define SERVICES_SERVICE_MANAGER_PUBLIC_CPP_TEST_TEST_SERVICE_MANAGER_H_

#include <memory>

#include "base/macros.h"
#include "base/values.h"
#include "services/service_manager/public/mojom/service.mojom.h"

namespace service_manager {

class BackgroundServiceManager;

// Creates a real ServiceManager instance for use in unit tests which want such
// a thing. Tests should use this class instead of depending on and constructing
// a |service_manager::ServiceManager| instance directly.
//
// NOTE: Using this object in tests requires a task execution environment, for
// example a live |base::test::ScopedTaskEnvironment| object.
class TestServiceManager {
 public:
  // Creates a TestServiceManager using the default global catalog.
  TestServiceManager();

  // Creates a TestServiceManager using the specific catalog contents in
  // |catalog.|
  explicit TestServiceManager(std::unique_ptr<base::Value> catalog);

  ~TestServiceManager();

  // Registers a new service instance with a random Identity including
  // |service_name|. Returns a ServiceRequest which the test fixture should
  // bind to a ServiceBinding it owns. This allows each test to behave as a
  // unique service instance with capabilities tied to |service_name|'s entry
  // in the testing catalog.
  //
  // TODO(https://crbug.com/895616): Support the caller supplying a manifest
  // object directly rather than supplying a service name and consulting a
  // global catalog.
  mojom::ServiceRequest RegisterTestInstance(const std::string& service_name);

 private:
  const std::unique_ptr<BackgroundServiceManager> background_service_manager_;

  DISALLOW_COPY_AND_ASSIGN(TestServiceManager);
};

}  // namespace service_manager

#endif  // SERVICES_SERVICE_MANAGER_PUBLIC_CPP_TEST_TEST_SERVICE_MANAGER_H_
