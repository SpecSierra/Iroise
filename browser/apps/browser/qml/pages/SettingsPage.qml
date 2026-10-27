/****************************************************************************
**
** Copyright (c) 2013 - 2021 Jolla Ltd.
** Copyright (c) 2020 - 2021 Open Mobile Platform LLC.
**
****************************************************************************/

/* This Source Code Form is subject to the terms of the Mozilla Public
 * License, v. 2.0. If a copy of the MPL was not distributed with this file,
 * You can obtain one at http://mozilla.org/MPL/2.0/. */

import QtQuick 2.6
import Sailfish.Silica 1.0
import harbour.iroise 1.0
import Nemo.Configuration 1.0
import Sailfish.Pickers 1.0
import "../shared" as Shared
import "components"

Page {
    id: page

    Shared.Background {
        anchors.fill: parent
        z: -1
    }

    // Color scheme constants (outside QtObject — QML forbids uppercase property names)
    readonly property int _prefersLightMode: 0
    readonly property int _prefersDarkMode: 1
    readonly property int _followsAmbience: 2

    QtObject {
        id: webEngineSettings
        property alias colorScheme: colorSchemeConf.value
    }
    ConfigurationValue { id: adBlockConf; key: "/apps/harbour-iroise/settings/adblock_enabled"; defaultValue: true }
    ConfigurationValue { id: cookieBannerConf; key: "/apps/harbour-iroise/settings/cookie_banner_blocking"; defaultValue: true }
    ConfigurationValue { id: gpcConf; key: "/apps/harbour-iroise/settings/global_privacy_control"; defaultValue: true }
    // 0 = light, 1 = dark, 2 = follow ambience (applied by SettingManager)
    ConfigurationValue { id: colorSchemeConf; key: "/apps/harbour-iroise/settings/color_scheme"; defaultValue: page._followsAmbience }

    readonly property int _textSwitchIconCenter: Math.round((Theme.iconSizeMedium - Theme.itemSizeExtraSmall) / 2.0)

    function removeProtocolTypeFromUri(uri) {
        if (uri.length === 0)
            return uri
        return uri.replace(/(^\w+:|^)\/\//, '')
    }

    SilicaFlickable {
        anchors.fill: parent
        contentHeight: contentColumn.height

        Column {
            id: contentColumn

            width: parent.width
            spacing: Theme.paddingMedium
            bottomPadding: Theme.paddingLarge

            PageHeader {
                //% "Settings"
                title: qsTrId("sailfish_browser-he-settings")
            }

            // Row order inside every section is by kind, not by importance:
            // every row that has an icon first -- value pickers, then the rows
            // that push a page -- and the switches last. Switches carry no icon
            // and are only indented to line their labels up with one, so any
            // switch above an icon row leaves a hole in the icon column.
            // General is the section people actually go looking in, which is
            // why Passwords sits here and "Close all tabs on exit" does not.
            SectionHeader {
                //: Section Header for the settings that are asked about most
                //% "General"
                text: qsTrId("iroise-sh-general")
            }

            BrowserComboBox {
                id: homePage

                readonly property bool _homePageBlank: homePageConfig.value === "about:blank"

                //: Label for home page text field
                //% "Home Page"
                label: qsTrId("settings_browser-la-home_page")
                //% "Start view"
                value: _homePageBlank ? qsTrId("sailfish_browser-la-start_view")
                                      : removeProtocolTypeFromUri(homePageConfig.value)

                currentIndex: _homePageBlank
                              ? 0 // For start view (blank)
                              : 1 // For web page

                iconSource: "image://theme/icon-m-home"

                on_HomePageBlankChanged: currentIndex = _homePageBlank ? 0 : 1

                menu: ContextMenu {
                    MenuItem {
                        //% "Start view"
                        text: qsTrId("sailfish_browser-me-start_view")
                        onClicked: homePageConfig.value = "about:blank"
                    }
                    MenuItem {
                        readonly property string site: removeProtocolTypeFromUri(homePageConfig.value)

                        property string title: {
                            if (homePage._homePageBlank || site === "") {
                                //: Shown when site is empty
                                //% "Web page"
                                return qsTrId("sailfish_browser-me-web_page_empty")
                            } else {
                                //: Instead of %1 site address will be displayed
                                //% "Web page %1"
                                return qsTrId("sailfish_browser-me-web_page").arg(site)
                            }
                        }

                        textFormat: Text.StyledText
                        color: highlighted ? Theme.highlightColor : Theme.primaryColor
                        text: Theme.highlightText(title, site, Theme.highlightColor)
                        onClicked: {
                            pageStack.animatorPush(Qt.resolvedUrl("components/AddHomePageDialog.qml"),
                                                   {homePageConfig: homePageConfig})
                        }
                    }
                }
            }

            BrowserComboBox {
                id: searchEngine

                //: Label for combobox that sets search engine used in browser
                //% "Search with"
                label: qsTrId("settings_browser-la-search_with")
                iconSource: "image://theme/icon-m-search"

                menu: ContextMenu {
                    Repeater {
                        model: SearchEngineModel
                        delegate: SearchEngineMenuItem {
                            text: title
                            //: Shown on Settings -> Search engine for user installable search services
                            //% "Tap to install"
                            description: status == SearchEngineModel.Available
                                         ? qsTrId("settings_browser-la-tap_to_install") : ""
                            onClicked: {
                                if (title !== searchEngineConfig.value) {
                                    if (status == SearchEngineModel.Available) {
                                        SearchEngineModel.install(title)
                                    } else {
                                        searchEngineConfig.value = title
                                    }
                                }
                            }

                            function syncCurrentIndex() {
                                if (text && (text === searchEngineConfig.value)) {
                                    searchEngine.currentIndex = index
                                }
                            }

                            // An engine installed from a website changes both
                            // its title (guess -> <ShortName>) and the setting,
                            // so neither can be checked once at creation.
                            onTextChanged: syncCurrentIndex()
                            Component.onCompleted: syncCurrentIndex()

                            Connections {
                                target: searchEngineConfig
                                onValueChanged: syncCurrentIndex()
                            }
                        }
                    }
                }
            }

            SettingsLinkItem {
                iconSource: "image://theme/icon-m-downloads"
                //% "Downloads"
                text: qsTrId("iroise-la-downloads")
                //% "Files saved to the Downloads folder"
                description: qsTrId("iroise-la-downloads_description")
                onClicked: pageStack.push("DownloadsPage.qml")
            }

            // Backed by the encrypted CredentialStore.
            SettingsLinkItem {
                iconSource: "image://theme/icon-m-keys"
                //: The label for the button for accessing password management
                //% "Passwords"
                text: qsTrId("settings_browser-la-passwords")
                //% "Saved logins, kept in an encrypted vault"
                description: qsTrId("iroise-la-passwords_description")
                onClicked: pageStack.push("LoginsPage.qml")
            }

            SectionHeader {
                //: Section Header for Appearance settings
                //% "Appearance"
                text: qsTrId("settings_browser-la-appearance")
            }

            BrowserComboBox {
                //% "Preferred color scheme"
                label: qsTrId("settings_browser-la-color_scheme")
                iconSource: "image://theme/icon-m-night"
                currentIndex: webEngineSettings.colorScheme

                //% "The website style to use when available"
                description: qsTrId("sailfish_browser-me-website_color_scheme")

                menu: ContextMenu {
                    MenuItem {
                        //: Option to prefer a website's light color scheme
                        //% "Light"
                        text: qsTrId("sailfish_browser-me-prefers_light_mode")
                        onClicked: webEngineSettings.colorScheme = page._prefersLightMode
                    }
                    MenuItem {
                        //: Option to prefer a website's dark color scheme
                        //% "Dark"
                        text: qsTrId("sailfish_browser-me-prefers_dark_mode")
                        onClicked: webEngineSettings.colorScheme = page._prefersDarkMode
                    }
                    MenuItem {
                        //: Option for the website's color scheme to match the ambience
                        //% "Match ambience"
                        text: qsTrId("sailfish_browser-me-follow_ambience")
                        onClicked: webEngineSettings.colorScheme = page._followsAmbience
                    }
                }
            }

            TextSwitch {
                //: Setting for fixed or dynamically shown/hidden toolbar
                //% "Fixed toolbar"
                text: qsTrId("settings_browser-la-fixed_toolbar")
                //% "Always show the bottom toolbar"
                description: qsTrId("settings_browser-la-fixed_toolbar_description")
                checked: fixedToolbarConfig.value
                // Margins adjusted to align with other items on the page
                leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge + _textSwitchIconCenter
                _label.anchors.leftMargin: Theme.paddingMedium + _textSwitchIconCenter
                automaticCheck: false
                onClicked: fixedToolbarConfig.value = !fixedToolbarConfig.value
            }

            SectionHeader {
                //: Section Header for privacy and security settings
                //% "Privacy & security"
                text: qsTrId("iroise-sh-privacy_security")
            }

            TextSwitch {
                //: Label for text switch that makes all tabs closed upon closing browser application
                //% "Close all tabs on exit"
                text: qsTrId("settings_browser-la-close_all_tabs")
                //% "When Browser is started next time, selected home page will be loaded"
                description: qsTrId("settings_browser-la-close_all_tabs_description")
                checked: closeAllTabsConfig.value
                // Margins adjusted to align with other items on the page
                leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge + _textSwitchIconCenter
                _label.anchors.leftMargin: Theme.paddingMedium + _textSwitchIconCenter
                automaticCheck: false

                onClicked: closeAllTabsConfig.value = !closeAllTabsConfig.value
            }

            TextSwitch {
                //% "Start browser in private browsing mode"
                text: qsTrId("settings_browser-la-start_in_private")
                checked: autoStartPrivateBrowser.value
                automaticCheck: false
                // Margins adjusted to align with other items on the page
                leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge + _textSwitchIconCenter
                _label.anchors.leftMargin: Theme.paddingMedium + _textSwitchIconCenter

                onClicked: autoStartPrivateBrowser.value = !autoStartPrivateBrowser.value
            }

            TextSwitch {
                //% "Adblock"
                text: qsTrId("iroise-la-ad_block")
                //% "Block advertisements and trackers"
                description: qsTrId("iroise-la-ad_block_description")
                checked: adBlockConf.value
                automaticCheck: false
                leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge + _textSwitchIconCenter
                _label.anchors.leftMargin: Theme.paddingMedium + _textSwitchIconCenter
                onClicked: {
                    // dconf is the single source of truth: BrowserPage's
                    // ConfigurationValue pushes the change into the engine via
                    // webView.setAdBlockEnabled. The old extra
                    // settingManager.setAdBlockEnabled(!adBlockConf.value)
                    // call read the value AFTER the flip, so it set the engine
                    // to the INVERSE of the visible switch state.
                    adBlockConf.value = !adBlockConf.value
                }
            }

            TextSwitch {
                //% "Block cookie banners"
                text: qsTrId("iroise-la-cookie_banner_blocking")
                //% "Automatically refuse cookie consent pop-ups"
                description: qsTrId("iroise-la-cookie_banner_blocking_description")
                checked: cookieBannerConf.value
                automaticCheck: false
                leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge + _textSwitchIconCenter
                _label.anchors.leftMargin: Theme.paddingMedium + _textSwitchIconCenter
                // dconf is the single source of truth; BrowserPage's
                // ConfigurationValue pushes the change process-wide.
                onClicked: cookieBannerConf.value = !cookieBannerConf.value
            }

            TextSwitch {
                //% "Ask sites not to sell my data"
                text: qsTrId("iroise-la-global_privacy_control")
                //% "Sends the Global Privacy Control signal, a legal opt-out in some places"
                description: qsTrId("iroise-la-global_privacy_control_description")
                checked: gpcConf.value
                automaticCheck: false
                leftMargin: Theme.horizontalPageMargin + Theme.paddingLarge + _textSwitchIconCenter
                _label.anchors.leftMargin: Theme.paddingMedium + _textSwitchIconCenter
                // dconf is the single source of truth; BrowserPage's
                // ConfigurationValue pushes the change process-wide.
                onClicked: gpcConf.value = !gpcConf.value
            }

            SectionHeader {
                //: Section Header for settings most people never need to open
                //% "Advanced"
                text: qsTrId("iroise-sh-advanced")
            }

            SettingsLinkItem {
                iconSource: "image://theme/icon-m-delete"
                //: The label for the button for accessing clear browsing data page
                //% "Clear browsing data"
                text: qsTrId("settings_browser-la-clear-browsing-data")
                //% "History, cookies, caches and site data"
                description: qsTrId("iroise-la-clear_browsing_data_description")
                onClicked: pageStack.push("PrivacySettingsPage.qml", { previousPage: page })
            }

            SettingsLinkItem {
                iconSource: "image://theme/icon-m-wizard"
                //% "Change user agents"
                text: qsTrId("iroise-he-site_user_agents")
                //% "Make selected websites see a different browser"
                description: qsTrId("iroise-he-site_user_agents_description")
                onClicked: pageStack.push("SiteUaSettingsPage.qml")
            }

        }
    }

    ConfigurationValue {
        id: closeAllTabsConfig

        key: "/apps/harbour-iroise/settings/close_all_tabs"
        defaultValue: false
    }

    ConfigurationValue {
        id: searchEngineConfig

        key: "/apps/harbour-iroise/settings/search_engine"
        defaultValue: "Google"
    }

    ConfigurationValue {
        id: homePageConfig

        key: "/apps/harbour-iroise/settings/home_page"
        defaultValue: "https://jolla.com/"
    }

    ConfigurationValue {
        id: fixedToolbarConfig

        key: "/apps/harbour-iroise/settings/fixed_toolbar"
        defaultValue: false
    }

    ConfigurationValue {
        id: autoStartPrivateBrowser

        key: "/apps/harbour-iroise/settings/browser_privatebrowsing_autostart"
        defaultValue: false
    }

    Notice {
        id: searchInstalledNotice

        duration: 3000
        verticalOffset: -Theme.paddingLarge
    }

    Connections {
        target: SearchEngineModel

        onInstalled: {
            // Installing happens because the user picked the engine in "Search
            // with", so finish that choice: the title here is the description's
            // own <ShortName>, which is what the setting stores.
            searchEngineConfig.value = title
            //% "%1 search installed"
            searchInstalledNotice.text = qsTrId("sailfish_browser-la-search_installed").arg(title)
            searchInstalledNotice.show()
        }

        onInstallFailed: {
            //: Shown when a search engine offered by a website could not be installed
            //% "Could not install %1 search"
            searchInstalledNotice.text = qsTrId("sailfish_browser-la-search_install_failed").arg(title)
            searchInstalledNotice.show()
        }
    }
}
