#pragma once
#include "api/models.hpp"
#include <QObject>
#include <QUrl>
#include <memory>

namespace gost::api {
class APIClient : public QObject {
public:
	explicit APIClient(Limits limits = {}, QObject *parent = nullptr);
	~APIClient() override;
	void configure(QUrl origin, RequestContext context);
	void cancelAll();
	void shutdown();
	void getReady(QObject *receiver, Callback<Health> callback);
	void getLive(QObject *receiver, Callback<Health> callback);
	void getCapabilities(QObject *receiver, Callback<Capabilities> callback);
	void getSnapshot(QObject *receiver, Callback<Anchor> callback);
	void getStream(QString id, QObject *receiver, Callback<Stream> callback);
	void getDestination(QString id, QObject *receiver, Callback<Destination> callback);
	void getProfile(QString id, QObject *receiver, Callback<Profile> callback);
	void getGroup(QString id, QObject *receiver, Callback<Group> callback);
	void getSession(QString id, QObject *receiver, Callback<Session> callback);
	void getCommand(QString id, QObject *receiver, Callback<Receipt> callback);
	void getAsset(QString id, QObject *receiver, Callback<Asset> callback);
	void getGeneration(QString assetId, QString generationId, QObject *receiver, Callback<Generation> callback);
	void getFallbackPolicy(QString streamId, QObject *receiver, Callback<PolicyResult> callback);
	void getSessionMetrics(QString sessionId, QObject *receiver, Callback<SessionMetrics> callback);
	void getOutput(QString sessionId, QString outputId, QObject *receiver, Callback<OutputDetail> callback);
#define GOST_DECLARE_LIST(Name, Type) \
 void get##Name(QObject *receiver, Callback<Page<Type>> callback, ListQuery query = {}); \
 void getAll##Name(QObject *receiver, Callback<Collection<Type>> callback, ListQuery query = {});
	GOST_DECLARE_LIST(Streams, Stream)
	GOST_DECLARE_LIST(Destinations, Destination)
	GOST_DECLARE_LIST(Profiles, Profile)
	GOST_DECLARE_LIST(Groups, Group)
	GOST_DECLARE_LIST(Sessions, Session)
	GOST_DECLARE_LIST(Assets, Asset)
#undef GOST_DECLARE_LIST
	void getStreamDestinations(QString id, QObject *receiver, Callback<Page<Destination>> callback,
				   ListQuery query = {});
	void getAllStreamDestinations(QString id, QObject *receiver, Callback<Collection<Destination>> callback,
				      ListQuery query = {});
	void getStreamSessions(QString id, QObject *receiver, Callback<Page<Session>> callback, ListQuery query = {});
	void getAllStreamSessions(QString id, QObject *receiver, Callback<Collection<Session>> callback,
				  ListQuery query = {});
	void getOutputs(QString sessionId, QObject *receiver, Callback<Page<Output>> callback, ListQuery query = {});
	void getAllOutputs(QString sessionId, QObject *receiver, Callback<Collection<Output>> callback,
			   ListQuery query = {});
	void getVariants(QString assetId, QObject *receiver, Callback<Page<Variant>> callback, ListQuery query = {});
	void getAllVariants(QString assetId, QObject *receiver, Callback<Collection<Variant>> callback,
			    ListQuery query = {});

private:
	class Impl;
	std::unique_ptr<Impl> impl_;
};
} // namespace gost::api
