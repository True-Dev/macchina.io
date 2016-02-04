//
// BundleLoader.cpp
//
// $Id: //poco/1.6/OSP/src/BundleLoader.cpp#1 $
//
// Library: OSP
// Package: Bundle
// Module:  BundleLoader
//
// Copyright (c) 2007-2014, Applied Informatics Software Engineering GmbH.
// All rights reserved.
//
// SPDX-License-Identifier: Apache-2.0
//


#include "Poco/OSP/BundleLoader.h"
#include "Poco/OSP/BundleContextFactory.h"
#include "Poco/OSP/BundleActivator.h"
#include "Poco/OSP/BundleEvent.h"
#include "Poco/OSP/CodeCache.h"
#include "Poco/OSP/OSPException.h"
#include "Poco/File.h"
#include "Poco/Path.h"
#include "Poco/Timestamp.h"
#include "Poco/Environment.h"
#include "Poco/Exception.h"
#if defined(POCO_REQUIRE_LICENSE)
#include "Poco/Licensing/License.h"
#endif
#include <memory>
#include <algorithm>
#include <cctype>


using Poco::File;
using Poco::Path;
using Poco::Timestamp;
using Poco::Environment;
using Poco::Logger;


#define POCO_OSP_STRINGIFY(X) POCO_OSP_STRINGIFY_(X)
#define POCO_OSP_STRINGIFY_(X) #X


namespace Poco {
namespace OSP {


BundleLoader::BundleLoader(CodeCache& codeCache, BundleFactory::Ptr pBundleFactory, BundleContextFactory::Ptr pBundleContextFactory, const std::string& osName, const std::string& osArch, bool autoUpdateCodeCache):
	_nextBundleId(1),
	_codeCache(codeCache),
	_autoUpdateCodeCache(autoUpdateCodeCache),
	_pBundleFactory(pBundleFactory),
	_pBundleContextFactory(pBundleContextFactory),
	_osName(osName),
	_osArch(osArch),
	_logger(Logger::get("osp.core.BundleLoader"))
{
	makeValidFileName(_osName);
	makeValidFileName(_osArch);
}


BundleLoader::BundleLoader(CodeCache& codeCache, BundleFactory::Ptr pBundleFactory, BundleContextFactory::Ptr pBundleContextFactory, bool autoUpdateCodeCache):
	_nextBundleId(1),
	_codeCache(codeCache),
	_autoUpdateCodeCache(autoUpdateCodeCache),
	_pBundleFactory(pBundleFactory),
	_pBundleContextFactory(pBundleContextFactory),
#ifdef POCO_TARGET_OSNAME
	_osName(POCO_OSP_STRINGIFY(POCO_TARGET_OSNAME)),
#else
	_osName(Environment::osName()),
#endif
#ifdef POCO_TARGET_OSARCH
	_osArch(POCO_OSP_STRINGIFY(POCO_TARGET_OSARCH)),
#else
	_osArch(Environment::osArchitecture()),
#endif
	_logger(Logger::get("osp.core.BundleLoader"))
{
	makeValidFileName(_osName);
#if (POCO_OS == POCO_OS_MAC_OS_X) && (POCO_ARCH == POCO_ARCH_AMD64)
	// Environment::osArchitecture() may return "i386" since the
	// kernel may be 32-bit while the userland is 64-bit
	_osArch = "x86_64";
#else
	makeValidFileName(_osArch);
#endif
}


BundleLoader::~BundleLoader()
{
#if defined(POCO_OSP_STATIC)
	for (BundleActivatorFactoryMap::iterator it = _bundleActivatorFactories.begin(); it != _bundleActivatorFactories.end(); ++it)
	{
		delete it->second;
	}
#endif
}


Bundle::ConstPtr BundleLoader::findBundle(const std::string& symbolicName) const
{
	BundleMap::const_iterator it = _bundles.find(symbolicName);
	if (it != _bundles.end())
		return it->second.pBundle;
	else
		return Bundle::Ptr();
}


Bundle::ConstPtr BundleLoader::findBundle(int id) const
{
	BundleIdMap::const_iterator it = _bundleIds.find(id);
	if (it != _bundleIds.end())
		return it->second;
	else
		return Bundle::Ptr();
}

	
Bundle::Ptr BundleLoader::createBundle(const std::string& path)
{
	return _pBundleFactory->createBundle(*this, path);
}


Bundle::Ptr BundleLoader::loadBundle(const std::string& path)
{
	Bundle::Ptr pBundle(createBundle(path));
	loadBundle(pBundle);
	return pBundle;
}


void BundleLoader::loadBundle(Bundle::Ptr pBundle)
{
#if defined(POCO_REQUIRE_LICENSE)
	poco_verify_license(OSP);
#endif

	Poco::Mutex::ScopedLock lock(_mutex);

	const std::string& symbolicName = pBundle->symbolicName();
	BundleMap::const_iterator it = _bundles.find(symbolicName);
	if (it == _bundles.end())
	{
		BundleInfo info;
		info.pBundle  = pBundle;
		info.pContext = _pBundleContextFactory->createBundleContext(*this, pBundle, _events);
		_bundles[symbolicName] = info;
		_bundleIds[pBundle->id()] = pBundle;
		
		BundleEvent loadedEvent(pBundle, BundleEvent::EV_BUNDLE_LOADED);
		_events.bundleLoaded(this, loadedEvent);
	}
	else 
	{
		std::string msg(symbolicName);
		msg += " version ";
		msg += pBundle->version().toString();
		msg += " from '";
		msg += pBundle->path();
		msg += "' conflicts with already loaded version ";
		msg += it->second.pBundle->version().toString();
		msg += " from '";
		msg += it->second.pBundle->path();
		msg += "'";
		throw BundleVersionConflictException(msg);
	}
}


void BundleLoader::unloadBundle(Bundle::Ptr pBundle)
{
	if (pBundle->isStarted())
		throw BundleStateException("An active bundle cannot be unloaded", pBundle->symbolicName());

	if (pBundle->isExtensionBundle())
	{
		Bundle::Ptr pExtendedBundle = pBundle->extendedBundle();
		if (pExtendedBundle)
		{
			pExtendedBundle->removeExtensionBundle(pBundle);
		}
	}

	Poco::Mutex::ScopedLock lock(_mutex);

	BundleMap::iterator it = _bundles.find(pBundle->symbolicName());
	if (it != _bundles.end())
	{
		unloadActivator(it->second);
	}
	BundleEvent unloadedEvent(pBundle, BundleEvent::EV_BUNDLE_UNLOADED);
	if (it != _bundles.end())
	{
		_bundles.erase(it);
	}
	_bundleIds.erase(pBundle->id());
	_events.bundleUnloaded(this, unloadedEvent);
}


void BundleLoader::listBundles(std::vector<Bundle::Ptr>& bundles) const
{
	Poco::Mutex::ScopedLock lock(_mutex);

	bundles.clear();
	bundles.reserve(_bundles.size());
	for (BundleMap::const_iterator it = _bundles.begin(); it != _bundles.end(); ++it)
	{
		bundles.push_back(it->second.pBundle);
	}
}


void BundleLoader::resolveAllBundles()
{
	Poco::Mutex::ScopedLock lock(_mutex);

	for (BundleMap::iterator it = _bundles.begin(); it != _bundles.end(); ++it)
	{
		if (!it->second.pBundle->isResolved())
		{
			try
			{
				it->second.pBundle->resolve();
			}
			catch (Poco::Exception& exc)
			{
				std::string msg("Failed to resolve bundle ");
				msg += it->second.pBundle->symbolicName();
				msg += ": ";
				msg += exc.displayText();
				_logger.error(msg);
				
				BundleError error;
				error.pBundle = it->second.pBundle;
				error.targetState = Bundle::BUNDLE_RESOLVED;
				error.pException = &exc;
				bundleError(this, error);
			}
		}
	}
}


namespace
{
	struct RunLevelLess
	{
		bool operator () (const Bundle::Ptr& p1, const Bundle::Ptr& p2) const
		{
			return p1->runLevel() < p2->runLevel();
		}
	};
}


void BundleLoader::startAllBundles()
{
	std::vector<Bundle::Ptr> bundles;
	listBundles(bundles);
	std::sort(bundles.begin(), bundles.end(), RunLevelLess());
	
	for (std::vector<Bundle::Ptr>::iterator it = bundles.begin(); it != bundles.end(); ++it)
	{
		if ((*it)->state() == Bundle::BUNDLE_RESOLVED && !(*it)->lazyStart())
		{
			try
			{
				(*it)->start();
			}
			catch (Poco::Exception& exc)
			{
				std::string msg("Failed to start bundle ");
				msg += (*it)->symbolicName();
				msg += ": ";
				msg += exc.displayText();
				_logger.error(msg);
				
				BundleError error;
				error.pBundle = *it;
				error.targetState = Bundle::BUNDLE_ACTIVE;
				error.pException = &exc;
				bundleError(this, error);
			}
		}
	}
}


void BundleLoader::stopAllBundles()
{
	std::vector<Bundle::Ptr> bundles;
	sortBundles(bundles);
	for (std::vector<Bundle::Ptr>::iterator it = bundles.begin(); it != bundles.end(); ++it)
	{
		if ((*it)->isActive())
		{
			(*it)->stop();
		}
	}
}

	
void BundleLoader::unloadAllBundles()
{
	std::vector<Bundle::Ptr> bundles;
	sortBundles(bundles);
	for (std::vector<Bundle::Ptr>::iterator it = bundles.begin(); it != bundles.end(); ++it)
	{
		if ((*it)->isActive())
		{
			(*it)->stop();
		}
		unloadBundle(*it);
	}
}
	
	
void BundleLoader::resolveBundle(Bundle* pBundle)
{
	Poco::Mutex::ScopedLock lock(_mutex);

	poco_assert (!isResolving(pBundle));

	if (_logger.debug())
	{
		_logger.debug(std::string("Resolving bundle ") + pBundle->symbolicName());
	}

	_resolvingBundles.insert(pBundle);
	try
	{
		const BundleManifest::Dependencies& deps = pBundle->requiredBundles();
		for (BundleManifest::Dependencies::const_iterator it = deps.begin(); it != deps.end(); ++it)
		{
			resolveDependency(pBundle, *it);
		}
	}
	catch (...)
	{
		_resolvingBundles.erase(pBundle);
		throw;
	}
	_resolvingBundles.erase(pBundle);
	
	if (_logger.debug())
	{
		_logger.debug(std::string("Installing libraries for ") + pBundle->symbolicName());
	}
	installLibraries(pBundle);
	
	if (pBundle->isExtensionBundle())
	{
		Bundle::Ptr pExtendedBundle = pBundle->extendedBundle();
		if (pExtendedBundle)
		{
			if (!pExtendedBundle->isResolved())
			{
				if (_logger.information())
				{
					_logger.information(std::string("Extended bundle ") + pExtendedBundle->symbolicName() + " not yet resolved - resolving now.");
				}
				pExtendedBundle->resolve();
			}
			pExtendedBundle->addExtensionBundle(pBundle);
			if (_logger.information())
			{
				_logger.information(std::string("Bundle ") + pBundle->symbolicName() + " successfully extended bundle " + pExtendedBundle->symbolicName());
			}
		}
		else if (_logger.warning())
		{
			_logger.warning("Bundle " + pBundle->symbolicName() + " cannot extend unknown bundle " + pBundle->manifest().extendedBundle() + ".");
		}
	}
	
	if (_logger.information())
	{
		_logger.information(std::string("Bundle ") + pBundle->symbolicName() + " resolved");
	}
}

	
void BundleLoader::startBundle(Bundle* pBundle)
{
	Poco::Mutex::ScopedLock lock(_mutex);

	if (_logger.debug())
	{
		_logger.debug(std::string("Starting bundle ") + pBundle->symbolicName());
	}

	BundleMap::iterator it = _bundles.find(pBundle->symbolicName());
	if (it != _bundles.end())
	{
		startDependencies(pBundle);
		BundleActivator* pActivator = loadActivator(it->second);
		if (pActivator)
		{
			_logger.debug("Invoking BundleActivator::start()");
			pActivator->start(it->second.pContext);
		}
		if (_logger.information())
		{
			_logger.information(std::string("Bundle ") + pBundle->symbolicName() + " started");
		}
	}
	else throw BundleException("The bundle loader does not know the bundle", pBundle->symbolicName());
}

	
void BundleLoader::stopBundle(Bundle* pBundle)
{
	Poco::Mutex::ScopedLock lock(_mutex);

	if (_logger.debug())
	{
		_logger.debug(std::string("Stopping bundle ") + pBundle->symbolicName());
	}

	BundleMap::iterator it = _bundles.find(pBundle->symbolicName());
	if (it != _bundles.end())
	{
		BundleActivator* pActivator = pBundle->activator();
		if (pActivator)
		{
			_logger.debug("Invoking BundleActivator::stop()");
			pActivator->stop(it->second.pContext);
		}
		if (_logger.information())
		{
			_logger.information(std::string("Bundle ") + pBundle->symbolicName() + " stopped");
		}
	}
	else throw BundleException("The bundle loader does not know the bundle", pBundle->symbolicName());
}

	
void BundleLoader::uninstallBundle(Bundle* pBundle)
{
	Poco::Mutex::ScopedLock lock(_mutex);

	if (_logger.debug())
	{
		_logger.debug(std::string("Uninstalling bundle ") + pBundle->symbolicName());
	}

	if (pBundle->isExtensionBundle())
	{
		Bundle::Ptr pExtendedBundle = pBundle->extendedBundle();
		if (pExtendedBundle)
		{
			pExtendedBundle->removeExtensionBundle(pBundle);
		}
	}
		
	BundleMap::iterator it = _bundles.find(pBundle->symbolicName());
	if (it != _bundles.end())
	{
		unloadActivator(it->second);
	}
	uninstallLibraries(pBundle);
	
	File bundleFile(pBundle->path());
	bundleFile.remove(true);
	
	BundleEvent unloadedEvent(pBundle, BundleEvent::EV_BUNDLE_UNLOADED);
	if (it != _bundles.end())
	{
		_bundles.erase(it);
	}
	_bundleIds.erase(pBundle->id());
	_events.bundleUnloaded(this, unloadedEvent);
	
	if (_logger.information())
	{
		_logger.information(std::string("Bundle ") + pBundle->symbolicName() + " uninstalled");
	}
}


BundleContext::Ptr BundleLoader::contextForBundle(Bundle::ConstPtr pBundle) const
{
	Poco::Mutex::ScopedLock lock(_mutex);

	BundleMap::const_iterator it = _bundles.find(pBundle->symbolicName());
	if (it != _bundles.end())
		return it->second.pContext;
	else
		throw BundleException("The bundle loader does not know the bundle", pBundle->symbolicName());
}


void BundleLoader::resolveDependency(Bundle* pBundle, const BundleManifest::Dependency& dependency)
{
	Bundle::Ptr pDepBundle(findBundle(dependency.symbolicName));
	if (pDepBundle)
	{
		if (dependency.versions.isEmpty() || dependency.versions.isInRange(pDepBundle->version()))
		{
			if (!pDepBundle->isResolved())
			{
				if (!isResolving(pDepBundle))
				{
					pDepBundle->resolve();
				}
				else
				{
					std::string msg("Circular reference detected involving ");
					msg += pBundle->symbolicName();
					msg += " and ";
					msg += pDepBundle->symbolicName();
					throw BundleResolveException(msg);
				}
			}
		}
		else 
		{
			std::string msg(dependency.symbolicName);
			msg += ", requested: ";
			msg += dependency.versions.toString();
			msg += ", available: ";
			msg += pDepBundle->version().toString();
			throw BundleResolveException("Required bundle not available in requested version", msg);
		}
	}
	else throw BundleResolveException("Required bundle not available", dependency.symbolicName);
}


bool BundleLoader::isResolving(Bundle* pBundle) const
{
	return _resolvingBundles.find(pBundle) != _resolvingBundles.end();
}


void BundleLoader::startDependencies(Bundle* pBundle)
{
	const BundleManifest::Dependencies& deps = pBundle->requiredBundles();
	for (BundleManifest::Dependencies::const_iterator it = deps.begin(); it != deps.end(); ++it)
	{
		Bundle::Ptr pDepBundle(findBundle(it->symbolicName));
		if (pDepBundle)
		{
			if (!pDepBundle->isActive())
			{
				pDepBundle->start();
			}
		}
		else throw BundleResolveException("A required bundle is no longer available", it->symbolicName);
	}
}


BundleActivator* BundleLoader::loadActivator(BundleInfo& bundleInfo)
{
	BundleActivator* pActivator(0);
#if defined(POCO_OSP_STATIC)
	pActivator = bundleInfo.pBundle->activator();
	if (!pActivator)
	{
		const std::string& activatorClass = bundleInfo.pBundle->activatorClass();
		if (!activatorClass.empty())
		{
			BundleActivatorFactoryMap::const_iterator it = _bundleActivatorFactories.find(activatorClass);
			if (it == _bundleActivatorFactories.end()) throw Poco::NotFoundException("Unknown BundleActivator class", activatorClass);
			pActivator = it->second->createInstance();
			bundleInfo.pBundle->setActivator(pActivator);
		}
	}	
#else
	if (!bundleInfo.pClassLoader)
	{
		ActivatorClassLoaderPtr pClassLoader(new ActivatorClassLoader);
		bundleInfo.pClassLoader = pClassLoader;
		const std::string& activatorClass = bundleInfo.pBundle->activatorClass();
		if (!activatorClass.empty())
		{
			std::string libPath(libraryPathFor(bundleInfo.pBundle));
			if (_logger.debug())
			{
				_logger.debug(std::string("Loading library ") + libPath);
			}
			pClassLoader->loadLibrary(libPath);
			const ActivatorClassLoader::Meta& meta = pClassLoader->classFor(activatorClass);
			if (meta.canCreate())
			{
				pActivator = meta.create();
				meta.autoDelete(pActivator);
			}
			else
			{
				pActivator = &meta.instance();
			}
			bundleInfo.pBundle->setActivator(pActivator);
		}
	}
	else
	{
		pActivator = bundleInfo.pBundle->activator();
	}
#endif
	return pActivator;
}


void BundleLoader::unloadActivator(BundleInfo& bundleInfo)
{
#if !defined(POCO_OSP_STATIC)
	if (bundleInfo.pClassLoader)
	{
		if (!bundleInfo.pBundle->activatorClass().empty())
		{
			std::string libPath(libraryPathFor(bundleInfo.pBundle));
			if (_logger.debug())
			{
				_logger.debug(std::string("Unloading library ") + libPath);
			}
			bundleInfo.pClassLoader->unloadLibrary(libPath);
		}
		bundleInfo.pClassLoader = 0;
	}
#endif
}


void BundleLoader::installLibraries(Bundle* pBundle)
{
	std::vector<std::string> libs;
	listLibraries(pBundle, libs);
	for (std::vector<std::string>::iterator it = libs.begin(); it != libs.end(); ++it)
	{
		Path p(false);
		p.pushDirectory("bin");
		p.pushDirectory(_osName);
		p.pushDirectory(_osArch);
		p.setFileName(*it);

		if (_codeCache.hasLibrary(*it))
		{
			if (_autoUpdateCodeCache)
			{
				Poco::Timestamp cachedFileTS(_codeCache.libraryTimestamp(*it));
				Poco::Timestamp bundledFileTS(pBundle->storage().lastModified(p.toString(Path::PATH_UNIX)));
				if (cachedFileTS < bundledFileTS)
				{
					installLibrary(pBundle, p, &bundledFileTS);
				}
				else
				{
					if (_logger.debug())
					{
						_logger.debug(std::string("Same or newer version of library found in cache: ") + *it);
					}
				}
			}
			else
			{
				if (_logger.debug())
				{
					_logger.debug(std::string("Library found in cache: ") + *it);
				}
			}
		}
		else
		{
			installLibrary(pBundle, p);
		}
	}
}


void BundleLoader::installLibrary(Bundle* pBundle, const Poco::Path& p, const Poco::Timestamp* pTS)
{
	if (_logger.debug())
	{
		_logger.debug(std::string("Installing library ") + p.toString(Path::PATH_UNIX));
	}
	std::auto_ptr<std::istream> pStream(pBundle->storage().getResource(p.toString(Path::PATH_UNIX)));
	if (pStream.get())
	{
		_codeCache.installLibrary(p.getFileName(), *pStream);
		if (pTS)
		{
			File f(_codeCache.pathFor(p.getFileName(), false));
			try
			{
				f.setLastModified(*pTS);
			}
			catch (Poco::Exception& exc)
			{
				_logger.warning(Poco::format("Failed to set timestamp on %s: %s", p.toString(), exc.displayText()));
			}
		}
	}
}


void BundleLoader::uninstallLibraries(Bundle* pBundle)
{
	std::vector<std::string> libs;
	listLibraries(pBundle, libs);
	for (std::vector<std::string>::iterator it = libs.begin(); it != libs.end(); ++it)
	{
		if (_logger.debug())
		{
			_logger.debug(std::string("Uninstalling library ") + *it);
		}
		_codeCache.uninstallLibrary(*it);
	}
}


void BundleLoader::listLibraries(Bundle* pBundle, std::vector<std::string>& list)
{
	Path p(false);
	p.pushDirectory("bin");
	p.pushDirectory(_osName);
	p.pushDirectory(_osArch);
	pBundle->storage().list(p.toString(Path::PATH_UNIX), list);
}


const std::string& BundleLoader::libraryNameFor(Bundle* pBundle)
{
	if (!pBundle->activatorLibrary().empty())
		return pBundle->activatorLibrary();
	else
		return pBundle->symbolicName();
}


void BundleLoader::sortBundles(std::vector<Bundle::Ptr>& bundles) const
{
	bundles.clear();
	bundles.reserve(_bundles.size());

	std::vector<Bundle::Ptr> remaining;
	listBundles(remaining);
	// account for run level
	std::sort(remaining.begin(), remaining.end(), RunLevelLess());
	
	std::set<std::string> sorted;
	while (!remaining.empty())
	{
		std::vector<Bundle::Ptr>::iterator it = remaining.begin();
		while (it != remaining.end())
		{	
			bool hasDeps = false;
			if ((*it)->isResolved())
			{
				const BundleManifest::Dependencies& deps = (*it)->requiredBundles();
				for (BundleManifest::Dependencies::const_iterator itDep = deps.begin(); itDep != deps.end(); ++itDep)
				{
					if (sorted.find(itDep->symbolicName) == sorted.end())
					{
						hasDeps = true;
						break;
					}
				}
			}
			if (!hasDeps)
			{
				bundles.push_back(*it);
				sorted.insert((*it)->symbolicName());
				remaining.erase(it);
				it = remaining.begin();
			}
			else ++it;
		}
	}
	std::reverse(bundles.begin(), bundles.end());
}


std::string BundleLoader::libraryPathFor(Bundle* pBundle)
{
	return _codeCache.pathFor(libraryNameFor(pBundle));
}


void BundleLoader::makeValidFileName(std::string& name)
{
	for (std::string::iterator it = name.begin(); it != name.end(); ++it)
	{
		if (!std::isalnum(*it)) *it = '_';
	}
}


std::string BundleLoader::pathForLibrary(const std::string& libraryName)
{
	return _codeCache.pathFor(libraryName);
}


int BundleLoader::nextBundleId()
{
	int nextBundleId;
	{
		Poco::Mutex::ScopedLock lock(_mutex);
		nextBundleId = _nextBundleId++;
	}
	return nextBundleId;
}


#if defined(POCO_OSP_STATIC)
void BundleLoader::registerBundleActivator(const std::string& className, BundleActivatorFactory* pFactory)
{
	BundleActivatorFactoryMap::iterator it = _bundleActivatorFactories.find(className);
	if (it == _bundleActivatorFactories.end())
	{
		_bundleActivatorFactories[className] = pFactory;
	}
	else
	{
		delete pFactory;
		throw Poco::ExistsException("Factory for BundleActivator already registered", className);
	}
}
#endif


} } // namespace Poco::OSP
