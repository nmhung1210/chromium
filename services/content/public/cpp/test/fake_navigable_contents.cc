// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "services/content/public/cpp/test/fake_navigable_contents.h"

#include "base/bind.h"
#include "base/unguessable_token.h"
#include "services/content/public/cpp/navigable_contents_view.h"
#include "services/content/public/cpp/test/fake_navigable_contents_factory.h"

namespace content {

FakeNavigableContents::FakeNavigableContents() {
  // This test-only object should only be used in environments where there is
  // no Content Service. Hence we let the client think it's running in the
  // same process as this service. This makes it easier to support fake
  // behavior. Only needs to be called once, but there's no harm in calling it
  // multiple times. This is more convenient.
  NavigableContentsView::SetClientRunningInServiceProcess();
}

FakeNavigableContents::~FakeNavigableContents() = default;

void FakeNavigableContents::Bind(mojom::NavigableContentsRequest request,
                                 mojom::NavigableContentsClientPtr client) {
  binding_.Bind(std::move(request));
  client_ = std::move(client);
}

void FakeNavigableContents::Navigate(const GURL& url,
                                     mojom::NavigateParamsPtr params) {
  client_->DidFinishNavigation(url, true /* is_main_frame */,
                               false /* is_error_page */,
                               default_response_headers_);
  client_->DidStopLoading();
}

void FakeNavigableContents::GoBack(
    mojom::NavigableContents::GoBackCallback callback) {
  std::move(callback).Run(false /* success */);
}

void FakeNavigableContents::CreateView(bool in_service_process,
                                       CreateViewCallback callback) {
  auto token = base::UnguessableToken::Create();
  NavigableContentsView::RegisterInProcessEmbedCallback(token,
                                                        base::DoNothing());
  std::move(callback).Run(token);
}

}  // namespace content
