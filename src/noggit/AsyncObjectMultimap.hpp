// This file is part of Noggit3, licensed under GNU General Public License (version 3).

#pragma once
#include <noggit/AsyncLoader.h>
#include <noggit/AsyncObject.h>
#include <noggit/ContextObject.hpp>
#include <noggit/Model.h>

#include <Listfile.hpp>

#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>

struct pair_hash
{
  std::size_t operator() (const std::pair<int, BlizzardArchive::Listfile::FileKey> &p) const noexcept
  {
    auto h1 = std::hash<int>{}(p.first);
    auto h2 = std::hash<std::string>{}(p.second.hasFilepath() ? p.second.filepath() : "");
    auto h3 = std::hash<int>{}(p.second.fileDataID());

    return h1 ^ h2 ^ h3;
  }
};

namespace Noggit
{

  template<typename T>
  struct AsyncObjectMultimap
  {
    AsyncObjectMultimap() = default;
    ~AsyncObjectMultimap()
    {
      /*
      apply ( [&] (std::string const& key, T const&)
              {
                auto pair = std::make_pair(context, key);
                LogDebug << key << ": " << _counts.at(pair) << std::endl;
              }
            );
      */
    }

    template<typename... Args>
      T* emplace (BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context, Args&&... args)
    {
      std::scoped_lock const lock(_mutex);
      auto pair = std::make_pair(context, file_key);
      //LogDebug << "Emplacing " << normalized << " into context" << context << std::endl;

      {
        if ([&] { return _counts[pair]++; }())
        {
          return &_elements.at (pair);
        }
      }

      // count was 0: either a brand-new key, or a REVIVE of an entry whose erase is mid-flight
      // on another thread (see erase()). In the revive case the map emplace finds the existing
      // element and does NOT construct -- it must not be queued for load a second time.
      auto const emplaced ( _elements.emplace ( std::piecewise_construct
                                              , std::forward_as_tuple (pair)
                                              , std::forward_as_tuple (file_key.filepath(), context, args...)
                                              )
                          );
      T* const obj (&emplaced.first->second);

      if (emplaced.second)
      {
        AsyncLoader::instance->queue_for_load(static_cast<AsyncObject*>(obj));
      }

      return obj;
    }
    void erase (BlizzardArchive::Listfile::FileKey const& file_key, Noggit::NoggitRenderContext context)
    {
      auto pair = std::make_pair(context, file_key);
      //LogDebug << "Erasing " << normalized << " from context" << context << std::endl;

      // RACE-SENSITIVE: between "the refcount hit zero" and "the element is erased" the mutex
      // must be dropped (ensure_deletable blocks on the loader threads and must not hold it).
      // Two independent hazards live in that window, both captured in Stormwind crash dumps:
      //  1. REVIVE: another thread emplace()s the same key -- the map hands back the EXISTING
      //     object. Erasing it anyway leaves the reviver holding a freed object (captured as a
      //     wide-char texture path scribbled over a live Qt animation object).
      //  2. DOUBLE-ERASE: with the refcount bouncing 0 -> 1 -> 0 (release, revive, release),
      //     TWO erasers can be legitimately in flight for the same key. The slower one then
      //     calls virtual methods on an object the faster one already destroyed (captured as a
      //     null-vtable virtual call, AV reading address 0x20).
      // Fix: the refcount decides under the lock; a TOMBSTONE (_erasing) makes the kill
      // exclusive to ONE eraser; after the unlocked wait the owner RE-CHECKS the refcount --
      // if the key was revived it aborts, re-queueing the object for load if the dequeue
      // already pulled it out of the loader (otherwise waiters would hang forever).

      AsyncObject* obj = nullptr;

      {
        std::scoped_lock lock(_mutex);

        auto it = _counts.find(pair);
        if (it == _counts.end() || it->second == 0)
        {
          // unknown key, or released more times than acquired -- nothing to do
          return;
        }

        if (--(it->second) != 0)
        {
          return;
        }

        if (!_erasing.insert(pair).second)
        {
          // another eraser already owns the kill for this key -- our reference is released
          // (count is 0), the owner will see it and finish the job. Touch nothing.
          return;
        }

        obj = static_cast<AsyncObject*>(&(_elements.at(pair)));
      }

      // always make sure an async object can be deleted before deleting it
      // (must not hold _mutex here -- this can block on the loader threads).
      // Deliberately UNCONDITIONAL: `finished` becomes true INSIDE finishLoading()/
      // error_on_loading() while the loader thread still touches the object afterwards
      // (is_required_when_saving() virtual call, _currently_loading removal) -- gating this
      // wait on finishedLoading() would let us destroy the object inside that window.
      // For a genuinely idle object this returns immediately.
      AsyncLoader::instance->ensure_deletable(obj);

      {
        std::scoped_lock lock(_mutex);

        _erasing.erase(pair);

        auto it = _counts.find(pair);
        if (it == _counts.end())
        {
          return;
        }

        if (it->second != 0)
        {
          // revived while the lock was dropped. If ensure_deletable dequeued the object before
          // it ever loaded, put it back -- the reviver believes it is loading.
          AsyncObject* revived = static_cast<AsyncObject*>(&(_elements.at(pair)));
          if (!revived->finishedLoading())
          {
            AsyncLoader::instance->queue_for_load(revived);
          }
          return;
        }

        _elements.erase(pair);
        _counts.erase(pair);
      }
    }
    void apply (std::function<void (BlizzardArchive::Listfile::FileKey const&, T&)> fun)
    {
      std::scoped_lock lock(_mutex);

      for (auto& element : _elements)
      {
        fun (element.first.second, element.second);
      }
    }
    void apply (std::function<void (BlizzardArchive::Listfile::FileKey const&, T const&)> fun) const
    {
      std::scoped_lock lock(_mutex);
      for (auto const& element : _elements)
      {
        fun (element.first.second, element.second);
      }
    }

    void context_aware_apply(std::function<void (BlizzardArchive::Listfile::FileKey const&, T&)> fun, Noggit::NoggitRenderContext context)
    {
      std::scoped_lock lock(_mutex);

      for (auto& element : _elements)
      {
        if (element.first.first != context)
          continue;

        fun (element.first.second, element.second);
      }
    }
    void context_aware_apply(std::function<void (BlizzardArchive::Listfile::FileKey const&, T const&)> fun, Noggit::NoggitRenderContext context) const
    {
      std::scoped_lock lock(_mutex);
      for (auto const& element : _elements)
      {
        if (element.first.first != context)
          continue;

        fun (element.first.second, element.second);
      }
    }

  private:
    std::unordered_map<std::pair<int, BlizzardArchive::Listfile::FileKey>, T, pair_hash> _elements;
    std::unordered_map<std::pair<int, BlizzardArchive::Listfile::FileKey>, std::size_t, pair_hash> _counts;
    // keys whose final-release erase is in flight (owner is between the two locked sections);
    // guarded by _mutex. See erase() for the revive / double-erase protocol.
    std::unordered_set<std::pair<int, BlizzardArchive::Listfile::FileKey>, pair_hash> _erasing;
    std::mutex mutable _mutex;
  };

}
