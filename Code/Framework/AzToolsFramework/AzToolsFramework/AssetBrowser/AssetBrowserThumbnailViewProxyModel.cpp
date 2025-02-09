/*
 * Copyright (c) Contributors to the Open 3D Engine Project.
 * For complete copyright and license terms please see the LICENSE at the root of this distribution.
 *
 * SPDX-License-Identifier: Apache-2.0 OR MIT
 *
 */
#include <AzCore/Utils/Utils.h>

#include <AzFramework/StringFunc/StringFunc.h>

#include <AzToolsFramework/API/EditorAssetSystemAPI.h>
#include <AzToolsFramework/AssetBrowser/Previewer/PreviewerBus.h>
#include <AzToolsFramework/AssetBrowser/Previewer/PreviewerFactory.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserFilterModel.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserModel.h>
#include <AzToolsFramework/AssetBrowser/AssetBrowserThumbnailViewProxyModel.h>
#include <AzToolsFramework/AssetBrowser/Entries/AssetBrowserEntry.h>
#include <AzToolsFramework/Thumbnails/ThumbnailerBus.h>

#include <AzQtComponents/Components/Widgets/AssetFolderThumbnailView.h>



namespace AzToolsFramework
{
    namespace AssetBrowser
    {
        AssetBrowserThumbnailViewProxyModel::AssetBrowserThumbnailViewProxyModel(QObject* parent)
            : QIdentityProxyModel(parent)
        {
        }

        AssetBrowserThumbnailViewProxyModel::~AssetBrowserThumbnailViewProxyModel() = default;

        QVariant AssetBrowserThumbnailViewProxyModel::data(const QModelIndex& index, int role) const
        {
            auto assetBrowserEntry = mapToSource(index).data(AssetBrowserModel::Roles::EntryRole).value<const AssetBrowserEntry*>();
            AZ_Assert(assetBrowserEntry, "Couldn't fetch asset entry for the given index.");
            if (!assetBrowserEntry)
            {
                return {};
            }

            switch (role)
            {
            case Qt::DecorationRole:
                {
                    // If this entry has a custom previewer, use its thumbnail
                    AZ::EBusAggregateResults<const PreviewerFactory*> factories;
                    PreviewerRequestBus::BroadcastResult(factories, &PreviewerRequests::GetPreviewerFactory, assetBrowserEntry);
                    for (const auto factory : factories.values)
                    {
                        if (factory)
                        {
                            SharedThumbnail thumbnail;

                            ThumbnailerRequestBus::BroadcastResult(
                                thumbnail, &ThumbnailerRequests::GetThumbnail, assetBrowserEntry->GetThumbnailKey());
                            AZ_Assert(thumbnail, "The shared thumbnail was not available from the ThumbnailerRequestBus.");
                            if (thumbnail && thumbnail->GetState() != Thumbnail::State::Failed)
                            {
                                return thumbnail->GetPixmap();
                            }
                        }
                    }

                    // No custom previewer, so find the icon from files
                    QString iconPathToUse;
                    AZ::EBusAggregateResults<SourceFileDetails> results;
                    AssetBrowserInteractionNotificationBus::BroadcastResult(
                        results,
                        &AssetBrowserInteractionNotificationBus::Events::GetSourceFileDetails,
                        assetBrowserEntry->GetFullPath().c_str());

                    auto it = AZStd::find_if(
                        results.values.begin(),
                        results.values.end(),
                        [](const SourceFileDetails& details)
                        {
                            return !details.m_sourceThumbnailPath.empty();
                        });

                    const bool isFolder = assetBrowserEntry->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder;

                    if (it != results.values.end() || isFolder)
                    {
                        static constexpr const char* FolderIconPath = "Icons/AssetBrowser/Folder_16.svg";
                        const char* resultPath = isFolder ? FolderIconPath : it->m_sourceThumbnailPath.c_str();
                        // its an ordered bus, though, so first one wins.
                        // we have to massage this though.  there are three valid possibilities
                        // 1. its a relative path to source, in which case we have to find the full path
                        // 2. its an absolute path, in which case we use it as-is
                        // 3. its an embedded resource, in which case we use it as is.

                        // is it an embedded resource or absolute path?
                        if ((resultPath[0] == ':') || (!AzFramework::StringFunc::Path::IsRelative(resultPath)))
                        {
                            iconPathToUse = QString::fromUtf8(resultPath);
                        }
                        else
                        {
                            // getting here means its a relative path.  Can we find the real path of the file?  This also searches in
                            // gems for sources.
                            bool foundIt = false;
                            AZ::Data::AssetInfo info;
                            AZStd::string watchFolder;
                            AssetSystemRequestBus::BroadcastResult(
                                foundIt, &AssetSystemRequestBus::Events::GetSourceInfoBySourcePath, resultPath, info, watchFolder);

                            AZ_WarningOnce(
                                "Asset Browser",
                                foundIt,
                                "Unable to find source icon file in any source folders or gems: %s\n",
                                resultPath);

                            if (foundIt)
                            {
                                // the absolute path is join(watchfolder, relativepath); // since its relative to the watch folder.
                                AZStd::string finalPath;
                                AzFramework::StringFunc::Path::Join(watchFolder.c_str(), info.m_relativePath.c_str(), finalPath);
                                iconPathToUse = QString::fromUtf8(finalPath.c_str());
                            }
                        }

                        // Return a default icon if no icon has been found
                    }
                    if (iconPathToUse.isEmpty())
                    {
                        static constexpr const char* DefaultFileIconPath = "Assets/Editor/Icons/AssetBrowser/Default_16.svg";
                        AZ::IO::FixedMaxPath engineRoot = AZ::Utils::GetEnginePath();
                        AZ_Assert(!engineRoot.empty(), "Engine Root not initialized");
                        iconPathToUse = (engineRoot / DefaultFileIconPath).c_str();
                    }
                    return iconPathToUse;
                }
            case Qt::ToolTipRole:
                {
                    return assetBrowserEntry->data(11).toString();
                }
            case static_cast<int>(AzQtComponents::AssetFolderThumbnailView::Role::IsExpandable):
                {
                    // We don't want to see children on folders in the thumbnail view
                    if (assetBrowserEntry->GetEntryType() == AssetBrowserEntry::AssetEntryType::Folder)
                    {
                        return false;
                    }

                    return rowCount(index) > 0;
                }
            case static_cast<int>(AzQtComponents::AssetFolderThumbnailView::Role::IsTopLevel):
                {
                    if (m_searchResultsMode)
                    {
                        auto isExactMatch =
                            index.data(static_cast<int>(AzQtComponents::AssetFolderThumbnailView::Role::IsExactMatch)).value<bool>();
                        return isExactMatch;
                    }
                    else
                    {
                        if (m_rootIndex.isValid())
                        {
                            return index.parent() == m_rootIndex;
                        }
                        else
                        {
                            return index.parent().isValid() && !index.parent().parent().isValid();
                        }
                    }
                }
            case static_cast<int>(AzQtComponents::AssetFolderThumbnailView::Role::IsVisible):
                {
                    auto isExactMatch =
                        index.data(static_cast<int>(AzQtComponents::AssetFolderThumbnailView::Role::IsExactMatch)).value<bool>();
                    return !m_searchResultsMode || (m_searchResultsMode && isExactMatch);
                }
            }

            return QAbstractProxyModel::data(index, role);
        }

        void AssetBrowserThumbnailViewProxyModel::SetRootIndex(const QModelIndex& index)
        {
            m_rootIndex = index;
        }

        bool AssetBrowserThumbnailViewProxyModel::GetShowSearchResultsMode() const
        {
            return m_searchResultsMode;
        }

        void AssetBrowserThumbnailViewProxyModel::SetShowSearchResultsMode(bool searchMode)
        {
            if (m_searchResultsMode != searchMode)
            {
                m_searchResultsMode = searchMode;
                beginResetModel();
                endResetModel();
            }
        }
    } // namespace AssetBrowser
} // namespace AzToolsFramework
