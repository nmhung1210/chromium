// Copyright 2018 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/previews/previews_lite_page_navigation_throttle.h"

#include <stdint.h>
#include <string>
#include <unordered_set>
#include <vector>

#include "base/callback.h"
#include "base/memory/weak_ptr.h"
#include "base/metrics/histogram_macros.h"
#include "base/rand_util.h"
#include "base/strings/safe_sprintf.h"
#include "base/strings/string_number_conversions.h"
#include "base/strings/string_util.h"
#include "base/task/post_task.h"
#include "base/time/time.h"
#include "build/build_config.h"
#include "chrome/browser/browser_process.h"
#include "chrome/browser/content_settings/cookie_settings_factory.h"
#include "chrome/browser/net/spdyproxy/data_reduction_proxy_chrome_settings.h"
#include "chrome/browser/net/spdyproxy/data_reduction_proxy_chrome_settings_factory.h"
#include "chrome/browser/page_load_metrics/metrics_web_contents_observer.h"
#include "chrome/browser/previews/previews_lite_page_decider.h"
#include "chrome/browser/previews/previews_service.h"
#include "chrome/browser/previews/previews_service_factory.h"
#include "chrome/browser/previews/previews_ui_tab_helper.h"
#include "chrome/browser/profiles/profile.h"
#include "components/base32/base32.h"
#include "components/content_settings/core/browser/cookie_settings.h"
#include "components/content_settings/core/common/cookie_settings_base.h"
#include "components/data_reduction_proxy/core/common/data_reduction_proxy_headers.h"
#include "components/previews/core/previews_experiments.h"
#include "components/previews/core/previews_lite_page_url_handler.h"
#include "content/public/browser/browser_context.h"
#include "content/public/browser/browser_task_traits.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/navigation_handle.h"
#include "content/public/browser/page_navigator.h"
#include "content/public/browser/web_contents.h"
#include "content/public/browser/web_contents_observer.h"
#include "content/public/browser/web_contents_user_data.h"
#include "content/public/common/referrer.h"
#include "crypto/sha2.h"
#include "net/base/escape.h"
#include "net/base/ip_address.h"
#include "net/base/url_util.h"
#include "net/http/http_request_headers.h"
#include "net/http/http_status_code.h"
#include "net/http/http_util.h"
#include "services/network/public/cpp/network_quality_tracker.h"
#include "ui/base/window_open_disposition.h"
#include "url/gurl.h"

namespace {

constexpr char kChromeProxyHeader[] = "chrome-proxy";

const base::TimeDelta kBlacklistDuration = base::TimeDelta::FromDays(30);

bool IsPrivateDomain(const GURL& url) {
  if (url.host().find(".") == base::StringPiece::npos)
    return true;

  // Allow localhost check to be skipped if needed, like in testing.
  if (net::IsLocalhost(url))
    return !previews::params::LitePagePreviewsTriggerOnLocalhost();

  net::IPAddress ip_addr;
  if (url.HostIsIPAddress() && ip_addr.AssignFromIPLiteral(url.host()) &&
      !ip_addr.IsPubliclyRoutable()) {
    return true;
  }
  return false;
}

content::OpenURLParams MakeOpenURLParams(content::NavigationHandle* handle,
                                         GURL url,
                                         const std::string& headers) {
  content::OpenURLParams url_params(
      url, handle->GetReferrer(), WindowOpenDisposition::CURRENT_TAB,
      handle->GetPageTransition(), handle->IsRendererInitiated());
  url_params.extra_headers = headers;
  url_params.redirect_chain = handle->GetRedirectChain();
  url_params.frame_tree_node_id = handle->GetFrameTreeNodeId();
  url_params.user_gesture = handle->HasUserGesture();
  url_params.started_from_context_menu = handle->WasStartedFromContextMenu();
  return url_params;
}

// This is called on |WillStartRequest| to check whether the given |handle| is
// for a navigation that was started by this preview. If it was, the penalty
// (time between the original navigation start and the navigation start of
// |handle|) is reported to PLM.
void MaybeReportNavigationRestartPenalty(content::NavigationHandle* handle) {
  PreviewsUITabHelper* ui_tab_helper =
      PreviewsUITabHelper::FromWebContents(handle->GetWebContents());
  if (!ui_tab_helper)
    return;

  previews::PreviewsUserData* previews_data =
      ui_tab_helper->GetPreviewsUserData(handle);
  if (!previews_data)
    return;

  previews::PreviewsUserData::ServerLitePageInfo* info =
      previews_data->server_lite_page_info();
  if (!info)
    return;

  DCHECK_GT(info->original_navigation_start, base::TimeTicks());
  base::TimeDelta penalty =
      handle->NavigationStart() - info->original_navigation_start;

  bool updated = page_load_metrics::MetricsWebContentsObserver::FromWebContents(
                     handle->GetWebContents())
                     ->ReportNavigationRestartPenalty(handle, penalty);
  DCHECK(updated);
  if (updated) {
    UMA_HISTOGRAM_MEDIUM_TIMES(
        "Previews.ServerLitePage.ReportedNavigationRestartPenalty", penalty);
  } else {
    UMA_HISTOGRAM_MEDIUM_TIMES(
        "Previews.ServerLitePage.NotReportedNavigationRestartPenalty", penalty);
  }
}

// Gets the ServerLitePageInfo struct from an existing attempted lite page
// navigation, if there is one. If not, returns nullptr.
std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo>
GetServerLitePageInfo(content::NavigationHandle* handle) {
  PreviewsUITabHelper* ui_tab_helper =
      PreviewsUITabHelper::FromWebContents(handle->GetWebContents());
  if (!ui_tab_helper)
    return nullptr;

  previews::PreviewsUserData* previews_data =
      ui_tab_helper->GetPreviewsUserData(handle);
  if (!previews_data)
    return nullptr;

  if (!previews_data->server_lite_page_info())
    return nullptr;

  return std::make_unique<previews::PreviewsUserData::ServerLitePageInfo>(
      *previews_data->server_lite_page_info());
}

// Gets the ServerLitePageInfo struct from an existing attempted lite page
// navigation, if there is one. If not, returns a new ServerLitePageInfo
// initialized with |handle| that is not associated with a PreviewsUserData.
std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo>
GetOrCreateServerLitePageInfo(content::NavigationHandle* handle) {
  std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo> info =
      GetServerLitePageInfo(handle);
  if (!info) {
    info.reset(new previews::PreviewsUserData::ServerLitePageInfo());
    info->original_navigation_start = handle->NavigationStart();
  }
  return info;
}

}  // namespace

class WebContentsLifetimeHelper
    : public content::WebContentsObserver,
      public content::WebContentsUserData<WebContentsLifetimeHelper> {
 public:
  explicit WebContentsLifetimeHelper(content::WebContents* web_contents)
      : content::WebContentsObserver(web_contents),
        web_contents_(web_contents),
        weak_factory_(this) {}

  // Keep track of all ongoing navigations in this WebContents.
  void DidStartNavigation(content::NavigationHandle* handle) override {
    DCHECK(handle);
    if (!handle->IsInMainFrame())
      return;

    navigations_.insert(handle);

    // Check if the starting navigation was reported as being caused by a
    // restart. Note: This could be a navigation to the litepages server, or to
    // the original URL.
    if (restarted_navigation_url_ == handle->GetURL()) {
      // Get a new page id.
      PreviewsService* previews_service = PreviewsServiceFactory::GetForProfile(
          Profile::FromBrowserContext(web_contents()->GetBrowserContext()));
      PreviewsLitePageNavigationThrottleManager* manager =
          previews_service->previews_lite_page_decider();
      uint64_t page_id = manager->GeneratePageID();

      // Create a new PreviewsUserData if needed.
      PreviewsUITabHelper* ui_tab_helper =
          PreviewsUITabHelper::FromWebContents(web_contents());
      previews::PreviewsUserData* previews_data =
          ui_tab_helper->CreatePreviewsUserDataForNavigationHandle(handle,
                                                                   page_id);

      // Set the lite page state on the user data.
      if (!info_) {
        info_ =
            std::make_unique<previews::PreviewsUserData::ServerLitePageInfo>();
        info_->original_navigation_start = handle->NavigationStart();
      }
      previews_data->set_server_lite_page_info(std::move(info_));

      // Reset member state.
      restarted_navigation_url_ = GURL();
    }
  }

  // Keep track of all ongoing navigations in this WebContents.
  void DidFinishNavigation(content::NavigationHandle* handle) override {
    DCHECK(handle);
    if (!handle->IsInMainFrame())
      return;

    if (navigations_.find(handle) != navigations_.end()) {
      navigations_.erase(handle);
    }
  }

  // This method should be called after some delay to cancel an ongoing previews
  // navigation. This method checks if the ongoing navigation is for the given
  // |url|, if so the |fallback_callback| is run.
  void CheckForHungNavigation(const GURL& url,
                              base::OnceClosure fallback_callback) {
    DCHECK_GE(2u, navigations_.size());
    if (navigations_.empty())
      return;

    content::NavigationHandle* handle = *navigations_.begin();
    if (handle->GetURL() != url)
      return;

    UMA_HISTOGRAM_ENUMERATION(
        "Previews.ServerLitePage.ServerResponse",
        PreviewsLitePageNavigationThrottle::ServerResponse::kTimeout);

    std::move(fallback_callback).Run();
  }

  base::WeakPtr<WebContentsLifetimeHelper> GetWeakPtr() {
    return weak_factory_.GetWeakPtr();
  }

  void PostNewNavigation(
      const content::OpenURLParams& url_params,
      std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo> info) {
    DCHECK_CURRENTLY_ON(content::BrowserThread::UI);
    DCHECK(url_params.url.is_valid());
    DCHECK(url_params.url.SchemeIs(url::kHttpsScheme));
    // Setting these members should always happen before |OpenURL| which can be
    // synchronous.
    restarted_navigation_url_ = url_params.url;
    info_ = std::move(info);

    web_contents_->OpenURL(url_params);
  }

 private:
  // The url to monitor for. When it is seen, |info_| will be attached to that
  // navigation.
  GURL restarted_navigation_url_;

  // The ServerLitePageInfo to attach to the next navigation that matches
  // |restarted_navigation_url_|.
  std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo> info_;

  content::WebContents* web_contents_;
  std::unordered_set<content::NavigationHandle*> navigations_;
  base::WeakPtrFactory<WebContentsLifetimeHelper> weak_factory_;
};

bool HandlePreviewsLitePageURLRewrite(
    GURL* url,
    content::BrowserContext* browser_context) {
  // Don't change the |url|, just register our interest in reversing it before
  // it is displayed to the user in |HandlePreviewsLitePageURLRewriteReverse|.
  return previews::IsLitePageRedirectPreviewURL(*url);
}

bool HandlePreviewsLitePageURLRewriteReverse(
    GURL* url,
    content::BrowserContext* browser_context) {
  std::string original_url;
  if (previews::ExtractOriginalURLFromLitePageRedirectURL(*url,
                                                          &original_url)) {
    *url = GURL(original_url);
    return true;
  }
  return false;
}

PreviewsLitePageNavigationThrottle::PreviewsLitePageNavigationThrottle(
    content::NavigationHandle* handle,
    PreviewsLitePageNavigationThrottleManager* manager)
    : content::NavigationThrottle(handle), manager_(manager) {
  DCHECK(manager_);
  DCHECK(handle);
  DCHECK(handle->GetWebContents());
  DCHECK(handle->GetWebContents()->GetBrowserContext());
}

PreviewsLitePageNavigationThrottle::~PreviewsLitePageNavigationThrottle() =
    default;

bool PreviewsLitePageNavigationThrottle::IsEligibleForPreview() const {
  DCHECK(navigation_handle()->IsInMainFrame());
  DCHECK_NE(navigation_handle()->GetReloadType(),
            content::ReloadType::ORIGINAL_REQUEST_URL);

  // Check if the parameters of the navigation are not eligible for the preview.
  std::vector<IneligibleReason> ineligible_reasons;
  const GURL& url = navigation_handle()->GetURL();
  if (!url.SchemeIs(url::kHttpsScheme))
    ineligible_reasons.push_back(IneligibleReason::kNonHttpsScheme);

  if (navigation_handle()->IsPost())
    ineligible_reasons.push_back(IneligibleReason::kHttpPost);

  if (manager_->IsServerUnavailable())
    ineligible_reasons.push_back(IneligibleReason::kServerUnavailable);

  if (g_browser_process->network_quality_tracker()
          ->GetEffectiveConnectionType() >
      previews::params::GetECTThresholdForPreview(
          previews::PreviewsType::LITE_PAGE_REDIRECT)) {
    ineligible_reasons.push_back(IneligibleReason::kNetworkNotSlow);
  }

  content_settings::CookieSettings* cookie_settings =
      CookieSettingsFactory::GetForProfile(
          Profile::FromBrowserContext(
              navigation_handle()->GetWebContents()->GetBrowserContext()))
          .get();
  ContentSetting setting;
  GURL previews_url = GetPreviewsURLForURL(url);
  cookie_settings->GetCookieSetting(previews_url, previews_url, nullptr,
                                    &setting);
  if (!content_settings::CookieSettingsBase::IsAllowed(setting)) {
    ineligible_reasons.push_back(IneligibleReason::kCookiesBlocked);
  }

  // Record UMA.
  for (IneligibleReason reason : ineligible_reasons) {
    UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.IneligibleReasons",
                              reason);
  }
  if (!ineligible_reasons.empty())
    return false;

  // Check dynamic blacklists.
  std::vector<BlacklistReason> blacklist_reasons;

  if (previews::IsLitePageRedirectPreviewDomain(url))
    blacklist_reasons.push_back(BlacklistReason::kNavigationToPreviewsDomain);

  if (IsPrivateDomain(url))
    blacklist_reasons.push_back(BlacklistReason::kNavigationToPrivateDomain);

  std::vector<std::string> blacklisted_path_suffixes =
      previews::params::LitePagePreviewsBlacklistedPathSuffixes();
  for (const std::string& suffix : blacklisted_path_suffixes) {
    if (base::EndsWith(url.path(), suffix,
                       base::CompareCase::INSENSITIVE_ASCII)) {
      blacklist_reasons.push_back(BlacklistReason::kPathSuffixBlacklisted);
      break;
    }
  }

  if (manager_->HostBlacklisted(url.host()))
    blacklist_reasons.push_back(BlacklistReason::kHostBlacklisted);

  // Record UMA
  for (BlacklistReason reason : blacklist_reasons) {
    UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.BlacklistReasons",
                              reason);
  }

  if (!blacklist_reasons.empty())
    return false;

  if (manager_->NeedsToNotifyUser()) {
    manager_->NotifyUser(navigation_handle()->GetWebContents());
    UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.IneligibleReasons",
                              IneligibleReason::kInfoBarNotSeen);
    return false;
  }
  return true;
}

// static
GURL PreviewsLitePageNavigationThrottle::GetPreviewsURLForURL(
    const GURL& original_url) {
  DCHECK(original_url.is_valid());
  std::string experiment_id =
      previews::params::LitePageRedirectPreviewExperiment();
  std::string experiment_query;
  if (!experiment_id.empty()) {
    experiment_query =
        "&x=" + net::EscapeQueryParamValue(experiment_id, true /* use_plus */);
  }

  std::string origin_hash = base::ToLowerASCII(base32::Base32Encode(
      crypto::SHA256HashString(
          original_url.scheme() + "://" + original_url.host() + ":" +
          base::IntToString(original_url.EffectiveIntPort())),
      base32::Base32EncodePolicy::OMIT_PADDING));
  GURL previews_host = previews::params::GetLitePagePreviewsDomainURL();
  GURL previews_url = GURL(
      previews_host.scheme() + "://" + origin_hash + "." +
      previews_host.host() +
      (previews_host.has_port() ? (":" + previews_host.port()) : "") + "/p?u=" +
      net::EscapeQueryParamValue(original_url.spec(), true /* use_plus */) +
      experiment_query);
  DCHECK(previews_url.is_valid());
  DCHECK_EQ(previews_host.scheme(), previews_url.scheme());
  return previews_url;
}

GURL PreviewsLitePageNavigationThrottle::GetPreviewsURL() const {
  DCHECK(!previews::IsLitePageRedirectPreviewDomain(
      navigation_handle()->GetURL()));
  return GetPreviewsURLForURL(navigation_handle()->GetURL());
}

// static
void PreviewsLitePageNavigationThrottle::LoadAndBypass(
    content::WebContents* web_contents,
    PreviewsLitePageNavigationThrottleManager* manager,
    const content::OpenURLParams& params,
    std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo> info,
    bool use_post_task) {
  DCHECK(web_contents);
  DCHECK(manager);

  manager->AddSingleBypass(params.url.spec());

  WebContentsLifetimeHelper::CreateForWebContents(web_contents);
  WebContentsLifetimeHelper* helper =
      WebContentsLifetimeHelper::FromWebContents(web_contents);

  if (!use_post_task) {
    helper->PostNewNavigation(params, std::move(info));
    return;
  }

  base::PostTaskWithTraits(
      FROM_HERE, {content::BrowserThread::UI},
      base::BindOnce(&WebContentsLifetimeHelper::PostNewNavigation,
                     helper->GetWeakPtr(), params, std::move(info)));
}

content::NavigationThrottle::ThrottleCheckResult
PreviewsLitePageNavigationThrottle::TriggerPreview() const {
  net::HttpRequestHeaders request_headers;
  content::BrowserContext* browser_context =
      navigation_handle()->GetWebContents()->GetBrowserContext();

  // Set DRP headers.
  DataReductionProxyChromeSettings* drp_settings =
      DataReductionProxyChromeSettingsFactory::GetForBrowserContext(
          browser_context);
  DCHECK(drp_settings);
  request_headers.MergeFrom(drp_settings->GetProxyRequestHeaders());

  // Set ECT header.
  request_headers.SetHeader(data_reduction_proxy::chrome_proxy_ect_header(),
                            net::GetNameForEffectiveConnectionType(
                                g_browser_process->network_quality_tracker()
                                    ->GetEffectiveConnectionType()));

  // Add in the page id to the chrome-proxy header.
  if (request_headers.HasHeader(kChromeProxyHeader)) {
    std::string header_value;
    request_headers.GetHeader(kChromeProxyHeader, &header_value);

    // 64 bit uint fits in 16 characters when represented in hexadecimal, but
    // there needs to be a trailing null terminated character in the buffer.
    char page_id_buffer[17];
    base::strings::SafeSPrintf(page_id_buffer, "%x",
                               manager_->GeneratePageID());
    header_value += ", pid=" + std::string(page_id_buffer);
    request_headers.SetHeader(kChromeProxyHeader, header_value);
  }

  content::WebContents* web_contents = navigation_handle()->GetWebContents();
  WebContentsLifetimeHelper::CreateForWebContents(web_contents);
  WebContentsLifetimeHelper* helper =
      WebContentsLifetimeHelper::FromWebContents(web_contents);

  // Post a delayed task to the WebContents helper. This task will check after a
  // timeout whether the previews navigation has finished (either in success or
  // failure). If not, the helper will stop the ongoing previews navigation and
  // load the original page.
  const base::TimeDelta timeout =
      previews::params::LitePagePreviewsNavigationTimeoutDuration();
  if (timeout > base::TimeDelta()) {
    base::PostDelayedTaskWithTraits(
        FROM_HERE, {content::BrowserThread::UI},
        base::BindOnce(
            &WebContentsLifetimeHelper::CheckForHungNavigation,
            helper->GetWeakPtr(), GetPreviewsURL(),
            base::BindOnce(
                &PreviewsLitePageNavigationThrottle::LoadAndBypass,
                base::Unretained(web_contents), manager_,
                MakeOpenURLParams(navigation_handle(),
                                  navigation_handle()->GetURL(), std::string()),
                GetOrCreateServerLitePageInfo(navigation_handle()), false)),
        timeout);
  }

  // The helper class and its weak pointer protect against the WebContents
  // dying in-between the PostTask and its execution, resulting in a use after
  // free crash. Since the helper is a WebContentsUserData, it will be
  // destroyed when the WebContents is and the task will not be executed.
  base::PostTaskWithTraits(
      FROM_HERE, {content::BrowserThread::UI},
      base::BindOnce(&WebContentsLifetimeHelper::PostNewNavigation,
                     helper->GetWeakPtr(),
                     MakeOpenURLParams(navigation_handle(), GetPreviewsURL(),
                                       request_headers.ToString()),
                     GetOrCreateServerLitePageInfo(navigation_handle())));

  return content::NavigationThrottle::CANCEL;
}

content::NavigationThrottle::ThrottleCheckResult
PreviewsLitePageNavigationThrottle::MaybeNavigateToPreview() const {
  const bool trigger =
      IsEligibleForPreview() &&
      !manager_->CheckSingleBypass(navigation_handle()->GetURL().spec());
  UMA_HISTOGRAM_BOOLEAN("Previews.ServerLitePage.Triggered", trigger);
  if (trigger)
    return TriggerPreview();
  return content::NavigationThrottle::PROCEED;
}

content::NavigationThrottle::ThrottleCheckResult
PreviewsLitePageNavigationThrottle::WillStartRequest() {
  MaybeReportNavigationRestartPenalty(navigation_handle());
  return MaybeNavigateToPreview();
}

content::NavigationThrottle::ThrottleCheckResult
PreviewsLitePageNavigationThrottle::WillRedirectRequest() {
  // WillRedirectRequest is called after the navigation_handle's URL has already
  // been set to the next location. So inspect the previous URL for the presence
  // of the previews server.
  const std::vector<GURL>& redirect_chain =
      navigation_handle()->GetRedirectChain();
  // |navigation_handle()->GetURL()| is always the last element in the redirect
  // chain. So if we've come here after a redirect, the length of
  // |redirect_chain| is at least 2.
  const GURL& previous_url = redirect_chain[redirect_chain.size() - 2];

  // If we are redirecting on a preview, count some UMA and proceed.
  std::string original_url;
  if (previews::ExtractOriginalURLFromLitePageRedirectURL(previous_url,
                                                          &original_url)) {
    // A redirect means one of two things: (1) there is no preview available for
    // this page and we should redirect back to the original page. (2) the
    // previews server is forwarding along a redirect from the origin. The
    // difference between the two is where the Location header is pointing. If
    // it is pointing towards the original page, it is considered a bypass.
    // Otherwise it is just a forwarded bypass.
    if (GURL(original_url) == navigation_handle()->GetURL()) {
      manager_->AddSingleBypass(navigation_handle()->GetURL().spec());
      UMA_HISTOGRAM_MEDIUM_TIMES(
          "Previews.ServerLitePage.HttpOnlyFallbackPenalty",
          base::TimeTicks::Now() - navigation_handle()->NavigationStart());
      UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.ServerResponse",
                                ServerResponse::kPreviewUnavailable);
    } else {
      UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.ServerResponse",
                                ServerResponse::kRedirect);
    }

    // Check if the original host should be blacklisted, as directed by the
    // server.
    const net::HttpResponseHeaders* response_headers =
        navigation_handle()->GetResponseHeaders();

    std::string chrome_proxy_header;
    bool blacklist_host =
        response_headers &&
        response_headers->EnumerateHeader(nullptr, kChromeProxyHeader,
                                          &chrome_proxy_header) &&
        chrome_proxy_header.find("host-blacklisted") != std::string::npos;

    if (blacklist_host)
      manager_->BlacklistHost(GURL(original_url).host(), kBlacklistDuration);

    UMA_HISTOGRAM_BOOLEAN("Previews.ServerLitePage.HostBlacklistedOnBypass",
                          blacklist_host);

    return content::NavigationThrottle::PROCEED;
  }

  return MaybeNavigateToPreview();
}

content::NavigationThrottle::ThrottleCheckResult
PreviewsLitePageNavigationThrottle::WillFailRequest() {
  std::string original_url;
  if (!previews::ExtractOriginalURLFromLitePageRedirectURL(
          navigation_handle()->GetURL(), &original_url)) {
    return content::NavigationThrottle::PROCEED;
  }

  UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.ServerResponse",
                            ServerResponse::kFailed);

  // The Preview was triggered but there was some irrecoverable issue (like
  // there is no network connection). Load the original page and let it go
  // through the normal process for whatever error it is.
  std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo> info =
      GetServerLitePageInfo(navigation_handle());
  DCHECK(info);
  LoadAndBypass(
      navigation_handle()->GetWebContents(), manager_,
      MakeOpenURLParams(navigation_handle(), GURL(original_url), std::string()),
      std::move(info), true);
  return content::NavigationThrottle::CANCEL;
}

content::NavigationThrottle::ThrottleCheckResult
PreviewsLitePageNavigationThrottle::WillProcessResponse() {
  std::string original_url;
  if (!previews::ExtractOriginalURLFromLitePageRedirectURL(
          navigation_handle()->GetURL(), &original_url)) {
    // Return early if this request was not for a Preview.
    return content::NavigationThrottle::PROCEED;
  }

  const net::HttpResponseHeaders* response_headers =
      navigation_handle()->GetResponseHeaders();

  // After this point, the given response is known to be for a Preview.
  // The Previews server will only send the following response codes: 200, 307,
  // 404, and 503. 200 and 307 should proceed as normal, 404 and 503 request the
  // client to load the original page instead because the server is not capable
  // of generating a lite page. All other response codes are treated as a 404.

  const int response_code = response_headers->response_code();

  if (response_code == net::HTTP_OK) {
    // Attempt to get the original content length and report it to Data Saver.
    const int64_t ofcl =
        data_reduction_proxy::GetDataReductionProxyOFCL(response_headers);
    if (ofcl > 0) {
      manager_->ReportDataSavings(response_headers->GetContentLength(), ofcl,
                                  GURL(original_url).host());
    }

    UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.ServerResponse",
                              ServerResponse::kOk);

    return content::NavigationThrottle::PROCEED;
  }

  const base::TimeDelta penalty =
      base::TimeTicks::Now() - navigation_handle()->NavigationStart();
  UMA_HISTOGRAM_MEDIUM_TIMES("Previews.ServerLitePage.HttpOnlyFallbackPenalty",
                             penalty);

  if (response_code == net::HTTP_SERVICE_UNAVAILABLE) {
    std::string retry_after_header;
    base::TimeDelta retry_after = base::TimeDelta::FromSeconds(
        base::RandInt(60, previews::params::PreviewServerLoadshedMaxSeconds()));
    if (response_headers->EnumerateHeader(nullptr, "retry-after",
                                          &retry_after_header)) {
      net::HttpUtil::ParseRetryAfterHeader(retry_after_header,
                                           base::Time::Now(), &retry_after);
    }
    manager_->SetServerUnavailableFor(retry_after);

    UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.ServerResponse",
                              ServerResponse::kServiceUnavailable);
  } else if (response_code == net::HTTP_FORBIDDEN) {
    UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.ServerResponse",
                              ServerResponse::kAuthFailure);
  } else {
    UMA_HISTOGRAM_ENUMERATION("Previews.ServerLitePage.ServerResponse",
                              ServerResponse::kOther);
  }

  std::unique_ptr<previews::PreviewsUserData::ServerLitePageInfo> info =
      GetServerLitePageInfo(navigation_handle());
  DCHECK(info);
  LoadAndBypass(
      navigation_handle()->GetWebContents(), manager_,
      MakeOpenURLParams(navigation_handle(), GURL(original_url), std::string()),
      std::move(info), true);
  return content::NavigationThrottle::CANCEL;
}

const char* PreviewsLitePageNavigationThrottle::GetNameForLogging() {
  return "PreviewsLitePageNavigationThrottle";
}
