// Copyright 2014 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/ui/views/apps/app_info_dialog/app_info_summary_panel.h"

#include <stddef.h>

#include <vector>

#include "base/bind.h"
#include "base/callback_forward.h"
#include "base/logging.h"
#include "base/strings/utf_string_conversions.h"
#include "chrome/browser/extensions/extension_service.h"
#include "chrome/browser/extensions/launch_util.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/browser/ui/views/chrome_layout_provider.h"
#include "chrome/grit/generated_resources.h"
#include "extensions/browser/extension_prefs.h"
#include "extensions/browser/extension_system.h"
#include "extensions/browser/path_util.h"
#include "extensions/common/constants.h"
#include "extensions/common/extension.h"
#include "extensions/common/manifest.h"
#include "extensions/common/manifest_handlers/shared_module_info.h"
#include "extensions/common/manifest_url_handlers.h"
#include "ui/base/l10n/l10n_util.h"
#include "ui/base/models/combobox_model.h"
#include "ui/gfx/geometry/insets.h"
#include "ui/views/controls/combobox/combobox.h"
#include "ui/views/controls/label.h"
#include "ui/views/controls/link.h"
#include "ui/views/layout/box_layout.h"
#include "ui/views/view.h"

// A model for a combobox selecting the launch options for a hosted app.
// Displays different options depending on the host OS.
class LaunchOptionsComboboxModel : public ui::ComboboxModel {
 public:
  LaunchOptionsComboboxModel();
  ~LaunchOptionsComboboxModel() override;

  extensions::LaunchType GetLaunchTypeAtIndex(int index) const;
  int GetIndexForLaunchType(extensions::LaunchType launch_type) const;

  // Overridden from ui::ComboboxModel:
  int GetItemCount() const override;
  base::string16 GetItemAt(int index) override;

 private:
  // A list of the launch types available in the combobox, in order.
  std::vector<extensions::LaunchType> launch_types_;

  // A list of the messages to display in the combobox, in order. The indexes in
  // this list correspond to the indexes in launch_types_.
  std::vector<base::string16> launch_type_messages_;
};

LaunchOptionsComboboxModel::LaunchOptionsComboboxModel() {
  // Hosted apps can only toggle between LAUNCH_TYPE_WINDOW and
  // LAUNCH_TYPE_REGULAR.
  launch_types_.push_back(extensions::LAUNCH_TYPE_REGULAR);
  launch_type_messages_.push_back(
      l10n_util::GetStringUTF16(IDS_APP_CONTEXT_MENU_OPEN_TAB));
  launch_types_.push_back(extensions::LAUNCH_TYPE_WINDOW);
  launch_type_messages_.push_back(
      l10n_util::GetStringUTF16(IDS_APP_CONTEXT_MENU_OPEN_WINDOW));
}

LaunchOptionsComboboxModel::~LaunchOptionsComboboxModel() {
}

extensions::LaunchType LaunchOptionsComboboxModel::GetLaunchTypeAtIndex(
    int index) const {
  return launch_types_[index];
}

int LaunchOptionsComboboxModel::GetIndexForLaunchType(
    extensions::LaunchType launch_type) const {
  for (size_t i = 0; i < launch_types_.size(); i++) {
    if (launch_types_[i] == launch_type) {
      return i;
    }
  }
  // If the requested launch type is not available, just select the first one.
  LOG(WARNING) << "Unavailable launch type " << launch_type << " selected.";
  return 0;
}

int LaunchOptionsComboboxModel::GetItemCount() const {
  return launch_types_.size();
}

base::string16 LaunchOptionsComboboxModel::GetItemAt(int index) {
  return launch_type_messages_[index];
}

AppInfoSummaryPanel::AppInfoSummaryPanel(Profile* profile,
                                         const extensions::Extension* app)
    : AppInfoPanel(profile, app),
      size_value_(NULL),
      homepage_link_(NULL),
      licenses_link_(NULL),
      launch_options_combobox_(NULL),
      weak_ptr_factory_(this) {
  SetLayoutManager(std::make_unique<views::BoxLayout>(
      views::BoxLayout::kVertical, gfx::Insets(),
      ChromeLayoutProvider::Get()->GetDistanceMetric(
          views::DISTANCE_RELATED_CONTROL_VERTICAL)));

  AddSubviews();
}

AppInfoSummaryPanel::~AppInfoSummaryPanel() {
  // Destroy view children before their models.
  RemoveAllChildViews(true);
}

void AppInfoSummaryPanel::AddDescriptionAndLinksControl(
    views::View* vertical_stack) {
  auto description_and_labels_stack = std::make_unique<views::View>();
  description_and_labels_stack->SetLayoutManager(
      std::make_unique<views::BoxLayout>(
          views::BoxLayout::kVertical, gfx::Insets(),
          ChromeLayoutProvider::Get()->GetDistanceMetric(
              DISTANCE_RELATED_CONTROL_VERTICAL_SMALL)));

  if (!app_->description().empty()) {
    const size_t max_length = 400;
    base::string16 text = base::UTF8ToUTF16(app_->description());
    if (text.length() > max_length) {
      text = text.substr(0, max_length);
      text += base::ASCIIToUTF16(" ... ");
    }

    auto description_label = std::make_unique<views::Label>(text);
    description_label->SetMultiLine(true);
    description_label->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    description_and_labels_stack->AddChildView(std::move(description_label));
  }

  if (CanShowAppHomePage()) {
    auto homepage_link = std::make_unique<views::Link>(
        l10n_util::GetStringUTF16(IDS_APPLICATION_INFO_HOMEPAGE_LINK));
    homepage_link->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    homepage_link->set_listener(this);
    homepage_link_ =
        description_and_labels_stack->AddChildView(std::move(homepage_link));
  }

  if (CanDisplayLicenses()) {
    auto licenses_link = std::make_unique<views::Link>(
        l10n_util::GetStringUTF16(IDS_APPLICATION_INFO_LICENSES_BUTTON_TEXT));
    licenses_link->SetHorizontalAlignment(gfx::ALIGN_LEFT);
    licenses_link->set_listener(this);
    licenses_link_ =
        description_and_labels_stack->AddChildView(std::move(licenses_link));
  }

  vertical_stack->AddChildView(std::move(description_and_labels_stack));
}

void AppInfoSummaryPanel::AddDetailsControl(views::View* vertical_stack) {
  // Component apps have no details.
  if (app_->location() == extensions::Manifest::COMPONENT)
    return;

  std::unique_ptr<views::View> details_list =
      CreateVerticalStack(ChromeLayoutProvider::Get()->GetDistanceMetric(
          DISTANCE_RELATED_CONTROL_VERTICAL_SMALL));

  // Add the size.
  auto size_title = std::make_unique<views::Label>(
      l10n_util::GetStringUTF16(IDS_APPLICATION_INFO_SIZE_LABEL));
  size_title->SetHorizontalAlignment(gfx::ALIGN_LEFT);

  auto size_value = std::make_unique<views::Label>(
      l10n_util::GetStringUTF16(IDS_APPLICATION_INFO_SIZE_LOADING_LABEL));
  size_value->SetHorizontalAlignment(gfx::ALIGN_LEFT);
  size_value_ = size_value.get();
  StartCalculatingAppSize();

  details_list->AddChildView(
      CreateKeyValueField(std::move(size_title), std::move(size_value)));

  // The version doesn't make sense for bookmark apps.
  if (!app_->from_bookmark()) {
    auto version_title = std::make_unique<views::Label>(
        l10n_util::GetStringUTF16(IDS_APPLICATION_INFO_VERSION_LABEL));
    version_title->SetHorizontalAlignment(gfx::ALIGN_LEFT);

    auto version_value = std::make_unique<views::Label>(
        base::UTF8ToUTF16(app_->GetVersionForDisplay()));
    version_value->SetHorizontalAlignment(gfx::ALIGN_LEFT);

    details_list->AddChildView(CreateKeyValueField(std::move(version_title),
                                                   std::move(version_value)));
  }

  vertical_stack->AddChildView(std::move(details_list));
}

void AppInfoSummaryPanel::AddLaunchOptionControl(views::View* vertical_stack) {
  if (!CanSetLaunchType())
    return;

  launch_options_combobox_model_.reset(new LaunchOptionsComboboxModel());
  auto launch_options_combobox =
      std::make_unique<views::Combobox>(launch_options_combobox_model_.get());
  launch_options_combobox->SetAccessibleName(
      l10n_util::GetStringUTF16(IDS_APPLICATION_INFO_LAUNCH_OPTIONS_ACCNAME));
  launch_options_combobox->set_listener(this);
  launch_options_combobox->SetSelectedIndex(
      launch_options_combobox_model_->GetIndexForLaunchType(GetLaunchType()));

  launch_options_combobox_ =
      vertical_stack->AddChildView(std::move(launch_options_combobox));
}

void AppInfoSummaryPanel::AddSubviews() {
  AddChildView(CreateHeading(
      l10n_util::GetStringUTF16(IDS_APPLICATION_INFO_APP_OVERVIEW_TITLE)));

  auto vertical_stack =
      CreateVerticalStack(ChromeLayoutProvider::Get()->GetDistanceMetric(
          views::DISTANCE_UNRELATED_CONTROL_VERTICAL));

  AddDescriptionAndLinksControl(vertical_stack.get());
  AddDetailsControl(vertical_stack.get());
  AddLaunchOptionControl(vertical_stack.get());

  AddChildView(std::move(vertical_stack));
}

void AppInfoSummaryPanel::OnPerformAction(views::Combobox* combobox) {
  if (combobox == launch_options_combobox_) {
    SetLaunchType(launch_options_combobox_model_->GetLaunchTypeAtIndex(
        launch_options_combobox_->selected_index()));
  } else {
    NOTREACHED();
  }
}

void AppInfoSummaryPanel::LinkClicked(views::Link* source, int event_flags) {
  if (source == homepage_link_) {
    ShowAppHomePage();
  } else if (source == licenses_link_) {
    DisplayLicenses();
  } else {
    NOTREACHED();
  }
}

void AppInfoSummaryPanel::StartCalculatingAppSize() {
  extensions::path_util::CalculateAndFormatExtensionDirectorySize(
      app_->path(), IDS_APPLICATION_INFO_SIZE_SMALL_LABEL,
      base::Bind(&AppInfoSummaryPanel::OnAppSizeCalculated, AsWeakPtr()));
}

void AppInfoSummaryPanel::OnAppSizeCalculated(const base::string16& size) {
  size_value_->SetText(size);
}

extensions::LaunchType AppInfoSummaryPanel::GetLaunchType() const {
  return extensions::GetLaunchType(extensions::ExtensionPrefs::Get(profile_),
                                   app_);
}

void AppInfoSummaryPanel::SetLaunchType(
    extensions::LaunchType launch_type) const {
  DCHECK(CanSetLaunchType());
  extensions::SetLaunchType(profile_, app_->id(), launch_type);
}

bool AppInfoSummaryPanel::CanSetLaunchType() const {
  // V2 apps and extensions don't have a launch type, and neither does the
  // Chrome app.
  return !app_->is_platform_app() && !app_->is_extension() &&
         app_->id() != extension_misc::kChromeAppId;
}
void AppInfoSummaryPanel::ShowAppHomePage() {
  DCHECK(CanShowAppHomePage());
  OpenLink(extensions::ManifestURL::GetHomepageURL(app_));
  Close();
}

bool AppInfoSummaryPanel::CanShowAppHomePage() const {
  return extensions::ManifestURL::SpecifiedHomepageURL(app_);
}

void AppInfoSummaryPanel::DisplayLicenses() {
  DCHECK(CanDisplayLicenses());
  for (const auto& license_url : GetLicenseUrls())
    OpenLink(license_url);
  Close();
}

bool AppInfoSummaryPanel::CanDisplayLicenses() const {
  return !GetLicenseUrls().empty();
}

const std::vector<GURL> AppInfoSummaryPanel::GetLicenseUrls() const {
  if (!extensions::SharedModuleInfo::ImportsModules(app_))
    return std::vector<GURL>();

  std::vector<GURL> license_urls;
  extensions::ExtensionService* service =
      extensions::ExtensionSystem::Get(profile_)->extension_service();
  DCHECK(service);
  const std::vector<extensions::SharedModuleInfo::ImportInfo>& imports =
      extensions::SharedModuleInfo::GetImports(app_);

  for (const auto& shared_module : imports) {
    const extensions::Extension* imported_module =
        service->GetExtensionById(shared_module.extension_id, true);
    DCHECK(imported_module);

    GURL about_page = extensions::ManifestURL::GetAboutPage(imported_module);
    if (about_page != GURL::EmptyGURL())
      license_urls.push_back(about_page);
  }
  return license_urls;
}