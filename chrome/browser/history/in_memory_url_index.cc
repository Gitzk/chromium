// Copyright (c) 2012 The Chromium Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "chrome/browser/history/in_memory_url_index.h"

#include "base/file_util.h"
#include "base/utf_string_conversions.h"
#include "chrome/browser/history/history_notifications.h"
#include "chrome/browser/history/url_database.h"
#include "chrome/browser/history/url_index_private_data.h"
#include "chrome/browser/profiles/profile.h"
#include "chrome/common/chrome_notification_types.h"
#include "chrome/common/url_constants.h"
#include "content/public/browser/browser_thread.h"
#include "content/public/browser/notification_details.h"
#include "content/public/browser/notification_service.h"
#include "content/public/browser/notification_source.h"

using in_memory_url_index::InMemoryURLIndexCacheItem;

namespace history {

// Called by DoSaveToCacheFile to delete any old cache file at |path| when
// there is no private data to save. Runs on the FILE thread.
void DeleteCacheFile(const FilePath& path) {
  DCHECK(!content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  file_util::Delete(path, false);
}

// Initializes a whitelist of URL schemes.
void InitializeSchemeWhitelist(std::set<std::string>* whitelist) {
  DCHECK(whitelist);
  if (!whitelist->empty())
    return;  // Nothing to do, already initialized.
  whitelist->insert(std::string(chrome::kAboutScheme));
  whitelist->insert(std::string(chrome::kChromeUIScheme));
  whitelist->insert(std::string(chrome::kFileScheme));
  whitelist->insert(std::string(chrome::kFtpScheme));
  whitelist->insert(std::string(chrome::kHttpScheme));
  whitelist->insert(std::string(chrome::kHttpsScheme));
  whitelist->insert(std::string(chrome::kMailToScheme));
}

// RefCountedBool --------------------------------------------------------------

RefCountedBool::~RefCountedBool() {}

// Restore/SaveCacheObserver ---------------------------------------------------

InMemoryURLIndex::RestoreCacheObserver::~RestoreCacheObserver() {}

InMemoryURLIndex::SaveCacheObserver::~SaveCacheObserver() {}

// RebuildPrivateDataFromHistoryDBTask -----------------------------------------

InMemoryURLIndex::RebuildPrivateDataFromHistoryDBTask::
    RebuildPrivateDataFromHistoryDBTask(
        InMemoryURLIndex* index,
        const std::string& languages,
        const std::set<std::string>& scheme_whitelist)
    : index_(index),
      languages_(languages),
      scheme_whitelist_(scheme_whitelist),
      succeeded_(false) {
}

bool InMemoryURLIndex::RebuildPrivateDataFromHistoryDBTask::RunOnDBThread(
    HistoryBackend* backend,
    HistoryDatabase* db) {
  data_ = URLIndexPrivateData::RebuildFromHistory(db, languages_,
                                                  scheme_whitelist_);
  succeeded_ = data_.get() && !data_->Empty();
  if (!succeeded_ && data_.get())
    data_->Clear();
  return true;
}

void InMemoryURLIndex::RebuildPrivateDataFromHistoryDBTask::
    DoneRunOnMainThread() {
  index_->DoneRebuidingPrivateDataFromHistoryDB(succeeded_, data_);
}

InMemoryURLIndex::RebuildPrivateDataFromHistoryDBTask::
    ~RebuildPrivateDataFromHistoryDBTask() {
}

// InMemoryURLIndex ------------------------------------------------------------

InMemoryURLIndex::InMemoryURLIndex(Profile* profile,
                                   const FilePath& history_dir,
                                   const std::string& languages)
    : profile_(profile),
      history_dir_(history_dir),
      languages_(languages),
      private_data_(new URLIndexPrivateData),
      restore_cache_observer_(NULL),
      save_cache_observer_(NULL),
      shutdown_(false),
      needs_to_be_cached_(false) {
  InitializeSchemeWhitelist(&scheme_whitelist_);
  if (profile) {
    // TODO(mrossetti): Register for language change notifications.
    content::Source<Profile> source(profile);
    registrar_.Add(this, chrome::NOTIFICATION_HISTORY_URL_VISITED, source);
    registrar_.Add(this, chrome::NOTIFICATION_HISTORY_URLS_MODIFIED,
                   source);
    registrar_.Add(this, chrome::NOTIFICATION_HISTORY_URLS_DELETED, source);
  }
}

// Called only by unit tests.
InMemoryURLIndex::InMemoryURLIndex()
    : profile_(NULL),
      private_data_(new URLIndexPrivateData),
      restore_cache_observer_(NULL),
      save_cache_observer_(NULL),
      shutdown_(false),
      needs_to_be_cached_(false) {
  InitializeSchemeWhitelist(&scheme_whitelist_);
}

InMemoryURLIndex::~InMemoryURLIndex() {
  // If there was a history directory (which there won't be for some unit tests)
  // then insure that the cache has already been saved.
  DCHECK(history_dir_.empty() || !needs_to_be_cached_);
}

void InMemoryURLIndex::Init() {
  PostRestoreFromCacheFileTask();
}

void InMemoryURLIndex::ShutDown() {
  registrar_.RemoveAll();
  cache_reader_consumer_.CancelAllRequests();
  shutdown_ = true;
  PostSaveToCacheFileTask();
  needs_to_be_cached_ = false;
}

void InMemoryURLIndex::ClearPrivateData() {
  private_data_->Clear();
}

bool InMemoryURLIndex::GetCacheFilePath(FilePath* file_path) {
  if (history_dir_.empty())
    return false;
  *file_path = history_dir_.Append(FILE_PATH_LITERAL("History Provider Cache"));
  return true;
}

// Querying --------------------------------------------------------------------

ScoredHistoryMatches InMemoryURLIndex::HistoryItemsForTerms(
    const string16& term_string) {
  return private_data_->HistoryItemsForTerms(term_string);
}

// Updating --------------------------------------------------------------------

void InMemoryURLIndex::Observe(int notification_type,
                               const content::NotificationSource& source,
                               const content::NotificationDetails& details) {
  switch (notification_type) {
    case chrome::NOTIFICATION_HISTORY_URL_VISITED:
      OnURLVisited(content::Details<URLVisitedDetails>(details).ptr());
      break;
    case chrome::NOTIFICATION_HISTORY_URLS_MODIFIED:
      OnURLsModified(
          content::Details<history::URLsModifiedDetails>(details).ptr());
      break;
    case chrome::NOTIFICATION_HISTORY_URLS_DELETED:
      OnURLsDeleted(
          content::Details<history::URLsDeletedDetails>(details).ptr());
      break;
    case chrome::NOTIFICATION_HISTORY_LOADED:
      registrar_.Remove(this, chrome::NOTIFICATION_HISTORY_LOADED,
                        content::Source<Profile>(profile_));
      ScheduleRebuildFromHistory();
      break;
    default:
      // For simplicity, the unit tests send us all notifications, even when
      // we haven't registered for them, so don't assert here.
      break;
  }
}

void InMemoryURLIndex::OnURLVisited(const URLVisitedDetails* details) {
  needs_to_be_cached_ |=
      private_data_->UpdateURL(details->row, languages_, scheme_whitelist_);
}

void InMemoryURLIndex::OnURLsModified(const URLsModifiedDetails* details) {
  for (URLRows::const_iterator row = details->changed_urls.begin();
       row != details->changed_urls.end(); ++row)
    needs_to_be_cached_ |=
        private_data_->UpdateURL(*row, languages_, scheme_whitelist_);
}

void InMemoryURLIndex::OnURLsDeleted(const URLsDeletedDetails* details) {
  if (details->all_history) {
    ClearPrivateData();
    needs_to_be_cached_ = true;
  } else {
    for (URLRows::const_iterator row = details->rows.begin();
         row != details->rows.end(); ++row)
      needs_to_be_cached_ |= private_data_->DeleteURL(row->url());
  }
}

// Restoring from Cache --------------------------------------------------------

void InMemoryURLIndex::PostRestoreFromCacheFileTask() {
  FilePath path;
  if (!GetCacheFilePath(&path) || shutdown_)
    return;
  scoped_refptr<URLIndexPrivateData> restored_private_data =
      new URLIndexPrivateData;
  content::BrowserThread::PostTaskAndReply(
      content::BrowserThread::FILE, FROM_HERE,
      base::Bind(&URLIndexPrivateData::RestoreFromFileTask, path,
          restored_private_data, languages_),
      base::Bind(&InMemoryURLIndex::OnCacheLoadDone, AsWeakPtr(),
          restored_private_data));
}

void InMemoryURLIndex::OnCacheLoadDone(
    scoped_refptr<URLIndexPrivateData> private_data) {
  if (private_data.get() && !private_data->Empty()) {
    private_data_ = private_data;
    if (restore_cache_observer_)
      restore_cache_observer_->OnCacheRestoreFinished(true);
  } else if (profile_) {
    // When unable to restore from the cache file delete the cache file, if
    // it exists, and then rebuild from the history database if it's available,
    // otherwise wait until the history database loaded and then rebuild.
    FilePath path;
    if (!GetCacheFilePath(&path) || shutdown_)
      return;
    content::BrowserThread::PostBlockingPoolTask(
        FROM_HERE, base::Bind(DeleteCacheFile, path));
    HistoryService* service = profile_->GetHistoryServiceWithoutCreating();
    if (service && service->backend_loaded()) {
      ScheduleRebuildFromHistory();
    } else {
      registrar_.Add(this, chrome::NOTIFICATION_HISTORY_LOADED,
                     content::Source<Profile>(profile_));
    }
  }
}

// Restoring from the History DB -----------------------------------------------

void InMemoryURLIndex::ScheduleRebuildFromHistory() {
  HistoryService* service =
      profile_->GetHistoryService(Profile::EXPLICIT_ACCESS);
  service->ScheduleDBTask(
      new InMemoryURLIndex::RebuildPrivateDataFromHistoryDBTask(
          this, languages_, scheme_whitelist_),
      &cache_reader_consumer_);
}

void InMemoryURLIndex::DoneRebuidingPrivateDataFromHistoryDB(
    bool succeeded,
    scoped_refptr<URLIndexPrivateData> private_data) {
  DCHECK(content::BrowserThread::CurrentlyOn(content::BrowserThread::UI));
  if (succeeded) {
    private_data_ = private_data;
    PostSaveToCacheFileTask();  // Cache the newly rebuilt index.
  } else {
    private_data_->Clear();  // Dump the old private data.
    // There is no need to do anything with the cache file as it was deleted
    // when the rebuild from the history operation was kicked off.
  }
  if (restore_cache_observer_)
    restore_cache_observer_->OnCacheRestoreFinished(succeeded);
}

void InMemoryURLIndex::RebuildFromHistory(HistoryDatabase* history_db) {
  private_data_ = URLIndexPrivateData::RebuildFromHistory(history_db,
                                                          languages_,
                                                          scheme_whitelist_);
}

// Saving to Cache -------------------------------------------------------------

void InMemoryURLIndex::PostSaveToCacheFileTask() {
  FilePath path;
  if (!GetCacheFilePath(&path))
    return;
  // If there is anything in our private data then make a copy of it and tell
  // it to save itself to a file.
  if (private_data_.get() && !private_data_->Empty()) {
    // Note that ownership of the copy of our private data is passed to the
    // completion closure below.
    scoped_refptr<URLIndexPrivateData> private_data_copy =
        private_data_->Duplicate();
    scoped_refptr<RefCountedBool> succeeded(new RefCountedBool(false));
    content::BrowserThread::PostTaskAndReply(
        content::BrowserThread::FILE, FROM_HERE,
        base::Bind(&URLIndexPrivateData::WritePrivateDataToCacheFileTask,
                   private_data_copy, path, succeeded),
        base::Bind(&InMemoryURLIndex::OnCacheSaveDone, AsWeakPtr(), succeeded));
  } else {
    // If there is no data in our index then delete any existing cache file.
    content::BrowserThread::PostBlockingPoolTask(
        FROM_HERE,
        base::Bind(DeleteCacheFile, path));
  }
}

void InMemoryURLIndex::OnCacheSaveDone(
    scoped_refptr<RefCountedBool> succeeded) {
  if (save_cache_observer_)
    save_cache_observer_->OnCacheSaveFinished(succeeded->value());
}

}  // namespace history
