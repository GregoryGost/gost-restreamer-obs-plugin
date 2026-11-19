#include "fixtures/fallback-rig.hpp"
#include <QApplication>
#include <QFile>
#include <QSignalSpy>
#include <QTest>

using namespace gost;
using namespace gost::api;

namespace {
// Возвращает наблюдаемые предпосылки допуска; сообщение отличает неполную фикстуру от отказа самого действия.
QString fallbackReadinessDiagnostic(const FallbackRig &test)
{
	// model содержит копию опубликованной карточки, policy — её адресную базу без секретов.
	const auto model = test.rig.state.fallback();
	const auto &policy = model.policy;
	return QStringLiteral(
		       "canWrite=%1 canPrepare=%2 canSavePolicy=%3 assetFresh=%4 loading=%5 writing=%6 selected=%7 asset=%8 pending=%9 policyLoading=%10 policyBase=%11 policyConflict=%12 settled=%13")
		.arg(test.fallback.canWrite())
		.arg(test.fallback.canPrepare())
		.arg(test.fallback.canSavePolicy())
		.arg(model.assetFresh)
		.arg(model.loading)
		.arg(model.writing)
		.arg(model.selectedAssetId)
		.arg(model.asset ? model.asset->id : QString{})
		.arg(model.asset ? model.asset->pendingGenerationId : QString{})
		.arg(policy.loading)
		.arg(policy.base.has_value())
		.arg(policy.serverConflict.has_value())
		.arg(test.rig.state.configurationSettled(test.rig.state.serverOrigin()));
}
} // namespace

// Проверяет наблюдаемые результаты через настоящие контроллеры и независимые HTTP-документы.
class FallbackControllerTest final : public QObject {
	Q_OBJECT
private slots:
	//202 освобождает запись до готовности; точные поколения и все страницы не смешиваются с current.
	void acceptedGenerationKeepsExactIdentityAndPages();
	// Чужие/повторные варианты и отказ второй страницы не подтверждают полный комплект.
	void invalidVariantWalk_data();
	// Дефект любого фрагмента обхода запрещает выдавать полный Ready.
	void invalidVariantWalk();
	// Ошибка нового pending сохраняет прежний current и уведомляет лишь о собственном принятом задании.
	void failedPendingPreservesCurrentAndDeduplicatesOwnFailure();
	// Исчерпание наблюдения не превращает Generation в error и сохраняет IDs для Refresh.
	void observationTimeoutKeepsAcceptance();
	// Задаёт незавершённую вторую страницу Variants и финальное чтение Asset после ready.
	void observationBudgetIncludesTerminalReads_data();
	// Общий срок отменяет всю цепочку, сохраняя принятую пару и разрешая лишь новый явный цикл GET.
	void observationBudgetIncludesTerminalReads();
	// Первое назначение использует согласованные интервалы, замена сохраняет все серверные значения.
	void policyUsesStreamRevisionAndPreservesDirtyName();
	// Только revision_conflict разрешает явное подтверждение новой CAS-базы.
	void policyConflict_data();
	// Ни один 409 не запускает автоматический повтор записи.
	void policyConflict();
	// Обновление страницы до завершения CAS GET сохраняет сравнение исходной, желаемой и серверной версии.
	void globalRefreshKeepsPendingPolicyComparison();
	// Задаёт все пять способов неоднозначной записи с разными обязательствами чтения.
	void cancelledWritesRequireFreshReconciliation_data();
	// Навигация, неполнота и ошибка GET не снимают общий барьер и не дают precheck/новый CRUD.
	void cancelledWritesRequireFreshReconciliation();
	// Неполный каталог после неизвестного create не доказывает завершённую сверку.
	void unknownCreateNeedsCompleteFreshCatalog();
	// Чужое pending после потери 202 не становится собственным принятием и удерживает барьер.
	void unknownPreparationKeepsUnownedPending();
	// Активная запись прежнего редактора блокирует новый fallback и оба пути будущих снимков.
	void configurationWriteBlocksFallbackAndBothPrechecks();
	// Session в паузе остаётся активной и запрещает новую подготовку без HTTP.
	void activeAndPausedSessionForbidPreparation_data();
	// Локальная проверка не меняет runtime и не посылает скрытую остановку OBS.
	void activeAndPausedSessionForbidPreparation();
	// Смена профиля и shutdown отзывают поздний 202, не отменяя чужую жизнь канала.
	void contextChangeAndShutdownRejectLateAcceptance();
	// Задаёт обоих владельцев записи, ожидающей чужой GET в общем канале.
	void contextChangeCancelsQueuedWriteBeforeFreeingSlot_data();
	// Смена профиля отзывает очередь до освобождения слота, поэтому неотправленная запись не становится Unknown.
	void contextChangeCancelsQueuedWriteBeforeFreeingSlot();
};

void FallbackControllerTest::acceptedGenerationKeepsExactIdentityAndPages()
{
	// test содержит прежний current; generationReads контролирует последовательные статусы собственного new.
	FallbackRig test;
	QVERIFY(test.initialize());
	int generationReads = 0;
	// newVariantPages считает обе страницы new, исключая варианты прежнего current.
	int newVariantPages = 0;
	// Перехватчик выдаёт queued/preparing/ready и две страницы только для точного new.
	test.intercept = [&](const auto &request) {
		if (request.target == "/api/v1/fallback-assets/asset/generations/new") {
			++generationReads;
			HttpServer::reply(request, fixture::bytes(fallbackFixture::generation(
							   QStringLiteral("new"),
							   generationReads == 1   ? QStringLiteral("queued")
							   : generationReads == 2 ? QStringLiteral("preparing")
										  : QStringLiteral("ready"))));
			return true;
		}
		// query извлекает закреплённую Generation и непрозрачный курсор текущей страницы.
		const QUrlQuery query(QUrl::fromEncoded("http://local" + request.target));
		if (request.target.startsWith("/api/v1/fallback-assets/asset/variants?") &&
		    query.queryItemValue(QStringLiteral("generation_id")) == QStringLiteral("new")) {
			++newVariantPages;
			HttpServer::reply(request,
					  fixture::page({fallbackFixture::variant(
								QStringLiteral("new"),
								query.queryItemValue(QStringLiteral("cursor")).isEmpty()
									? QStringLiteral("first")
									: QStringLiteral("second"))},
							query.queryItemValue(QStringLiteral("cursor")).isEmpty()
								? QStringLiteral("opaque /+%")
								: QString{}));
			return true;
		}
		return false;
	};
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	QTRY_COMPARE(test.rig.state.fallback().current.generationId, QStringLiteral("old"));
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Accepted);
	QVERIFY(!test.rig.state.fallback().writing);
	QVERIFY(test.rig.state.configurationSettled(test.rig.state.serverOrigin()));
	QCOMPARE(test.rig.state.fallback().acceptedGenerationId, QStringLiteral("new"));
	QTRY_VERIFY(test.rig.state.fallback().accepted.variantsStatus.complete);
	QTRY_COMPARE_WITH_TIMEOUT(test.rig.state.fallback().observation, FallbackObservation::Ready, 1500);
	QCOMPARE(generationReads, 3);
	QCOMPARE(newVariantPages, 2);
	QCOMPARE(test.rig.state.fallback().accepted.variants.size(), 2);
	QVERIFY(test.rig.state.fallback().accepted.variantsStatus.complete);
	QCOMPARE(test.rig.state.fallback().current.generationId, QStringLiteral("old"));
	QCOMPARE(test.rig.state.fallback().asset->currentGenerationId, QStringLiteral("old"));
	QCOMPARE(test.rig.mutations().size(), 1);
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackControllerTest::invalidVariantWalk_data()
{
	QTest::addColumn<QString>("violation");
	// violation задаёт отдельное противоречие публичной принадлежности или полноты страниц.
	for (const auto *violation : {"foreign-asset", "foreign-generation", "duplicate", "second-page-failed"})
		QTest::newRow(violation) << QString::fromLatin1(violation);
}

void FallbackControllerTest::invalidVariantWalk()
{
	// violation выбирает независимый дефект ответа; test сохраняет прежний current.
	QFETCH(QString, violation);
	FallbackRig test;
	QVERIFY(test.initialize());
	// Перехватчик меняет только варианты принятого поколения, моделируя независимое нарушение принадлежности либо полноты.
	test.intercept = [&](const auto &request) {
		// query фиксирует только варианты собственного принятого new.
		const QUrlQuery query(QUrl::fromEncoded("http://local" + request.target));
		if (!request.target.startsWith("/api/v1/fallback-assets/asset/variants?") ||
		    query.queryItemValue(QStringLiteral("generation_id")) != QStringLiteral("new"))
			return false;
		if (violation == QStringLiteral("second-page-failed") &&
		    !query.queryItemValue(QStringLiteral("cursor")).isEmpty()) {
			HttpServer::reply(request, "{}", 503);
			return true;
		}
		// value начинается валидным вариантом; каждый сценарий меняет ровно одно требование.
		auto value = fallbackFixture::variant(QStringLiteral("new"));
		if (violation == QStringLiteral("foreign-asset"))
			value.insert(QStringLiteral("asset_id"), QStringLiteral("other"));
		if (violation == QStringLiteral("foreign-generation"))
			value.insert(QStringLiteral("generation_id"), QStringLiteral("old"));
		HttpServer::reply(request,
				  fixture::page(violation == QStringLiteral("duplicate") ? QJsonArray{value, value}
											 : QJsonArray{value},
						violation == QStringLiteral("second-page-failed")
							? QStringLiteral("next")
							: QString{}));
		return true;
	};
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Accepted);
	QTRY_COMPARE(test.rig.state.fallback().observation, FallbackObservation::Unknown);
	QVERIFY(!test.rig.state.fallback().accepted.variantsStatus.complete);
	QCOMPARE(test.rig.state.fallback().current.generationId, QStringLiteral("old"));
	QCOMPARE(test.rig.mutations().size(), 1);
}

void FallbackControllerTest::failedPendingPreservesCurrentAndDeduplicatesOwnFailure()
{
	// test сначала показывает пассивную ошибку old; failures хранит только собственные уведомления.
	FallbackRig test;
	QVERIFY(test.initialize());
	QList<OperationFailure> failures;
	// Обработчик сохраняет только реально опубликованные отказы для проверки принадлежности и подавления повторов.
	connect(&test.fallback, &FallbackController::operationFailed, this,
		[&](const auto &failure) { failures.append(failure); });
	// failed содержит серверную структурированную причину с временной диагностикой.
	auto failed = fallbackFixture::generation(QStringLiteral("new"), QStringLiteral("error"));
	failed.insert(
		QStringLiteral("failure"),
		fixture::object(
			R"({"code":"preparation_failed","message":"Нет кодировщика","action":"Выберите профиль","field":"profile","context":{"asset_id":"asset"}})"));
	test.generations.insert(QStringLiteral("old"),
				fallbackFixture::generation(QStringLiteral("old"), QStringLiteral("error")));
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().current.generation.has_value());
	QCOMPARE(failures.size(), 0);
	test.generations.insert(QStringLiteral("old"),
				fallbackFixture::generation(QStringLiteral("old"), QStringLiteral("ready")));
	test.generations.insert(QStringLiteral("new"), failed);
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.state.fallback().observation, FallbackObservation::Error);
	QTRY_COMPARE(failures.size(), 1);
	QCOMPARE(failures[0].source, OperationFailureSource::Fallback);
	QCOMPARE(failures[0].code, QStringLiteral("preparation_failed"));
	QCOMPARE(test.rig.state.fallback().current.generationId, QStringLiteral("old"));
	QCOMPARE(test.rig.state.fallback().asset->currentGenerationId, QStringLiteral("old"));
	QVERIFY(test.rig.state.fallback().accepted.generation.has_value());
	QVERIFY(test.rig.state.fallback().accepted.generation->failure->diagnosticBody.isEmpty());
	test.fallback.refresh();
	QTRY_VERIFY(!test.rig.state.fallback().loading);
	QCOMPARE(failures.size(), 1);
	QCOMPARE(test.rig.mutations().size(), 1);
}

void FallbackControllerTest::observationTimeoutKeepsAcceptance()
{
	// test использует короткий цикл наблюдения; серверное queued остаётся неокончательным.
	FallbackRig test({70, 5});
	test.generations.insert(QStringLiteral("new"),
				fallbackFixture::generation(QStringLiteral("new"), QStringLiteral("queued")));
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.state.fallback().observation, FallbackObservation::Unknown);
	QCOMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Accepted);
	QCOMPARE(test.rig.state.fallback().acceptedGenerationId, QStringLiteral("new"));
	QVERIFY(test.rig.state.fallback().accepted.generation.has_value());
	QCOMPARE(test.rig.state.fallback().accepted.generation->status, QStringLiteral("queued"));
	test.generations.insert(QStringLiteral("new"),
				fallbackFixture::generation(QStringLiteral("new"), QStringLiteral("ready")));
	test.fallback.refresh();
	QTRY_COMPARE(test.rig.state.fallback().observation, FallbackObservation::Ready);
	QCOMPARE(test.rig.mutations().size(), 1);
}

void FallbackControllerTest::observationBudgetIncludesTerminalReads_data()
{
	QTest::addColumn<bool>("holdFinalAsset");
	QTest::newRow("ready-held-variants-page") << false;
	QTest::newRow("ready-held-final-asset") << true;
}

void FallbackControllerTest::observationBudgetIncludesTerminalReads()
{
	// holdFinalAsset выбирает шаг после достоверного ready, который не ответит до истечения общего срока.
	QFETCH(bool, holdFinalAsset);
	// limits сериализует чтения; test ограничивает весь цикл 500 мс, а отдельный REST допускает 5 секунд.
	Limits limits;
	limits.maxActiveRequests = 1;
	FallbackRig test({500, 5}, limits);
	QVERIFY(test.initialize());
	test.rig.configApi.setHttpTimeoutMs(5000);
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh && test.rig.state.fallback().current.variantsStatus.complete &&
		    !test.rig.state.fallback().loading);
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	// generationHeld удерживает первый GET принятой пары, чтобы убрать только независимый detail scope.
	std::optional<HttpServer::Request> generationHeld;
	// held фиксирует зависший шаг собственного наблюдения после ready.
	std::optional<HttpServer::Request> held;
	// generationReleased разрешает последующим явным циклам получать обычный серверный ответ.
	bool generationReleased = false;
	// acceptedVariantsServed отличает финальный Asset от чтений до приёма завершившегося поколения.
	bool acceptedVariantsServed = false;
	// generationReads обнаруживает автоматическое возобновление после исчерпания бюджета.
	int generationReads = 0;
	// Перехватчик сначала удерживает Generation, затем одну точную страницу либо финальный Asset этой цепочки.
	test.intercept = [&](const auto &request) {
		if (request.target == "/api/v1/fallback-assets/asset/generations/new") {
			++generationReads;
			if (!generationReleased) {
				generationHeld = request;
				return true;
			}
		}
		// query сохраняет принадлежность обеих страниц точному принятому поколению.
		const QUrlQuery query(QUrl::fromEncoded("http://local" + request.target));
		if (request.target.startsWith("/api/v1/fallback-assets/asset/variants?") &&
		    query.queryItemValue(QStringLiteral("generation_id")) == QStringLiteral("new")) {
			if (!holdFinalAsset && query.queryItemValue(QStringLiteral("cursor")).isEmpty()) {
				HttpServer::reply(request,
						  fixture::page({fallbackFixture::variant(QStringLiteral("new"),
											  QStringLiteral("first"))},
								QStringLiteral("next-page")));
				return true;
			}
			if (!holdFinalAsset && !held) {
				held = request;
				return true;
			}
			acceptedVariantsServed = true;
		}
		if (holdFinalAsset && acceptedVariantsServed && !held &&
		    request.target == "/api/v1/fallback-assets/asset") {
			held = request;
			return true;
		}
		return false;
	};
	QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_VERIFY_WITH_TIMEOUT(generationHeld.has_value(), 1500);
	QCOMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Accepted);
	QCOMPARE(test.rig.state.fallback().current.generationId, QStringLiteral("old"));
	// Навигация снимает только detail GET; принятая пара и её общий срок продолжают действовать.
	QVERIFY(test.fallback.selectAsset({}));
	generationReleased = true;
	HttpServer::reply(*generationHeld,
			  fixture::bytes(fallbackFixture::generation(QStringLiteral("new"), QStringLiteral("ready"))));
	QTRY_VERIFY_WITH_TIMEOUT(held.has_value(), 1500);
	QVERIFY(test.rig.state.fallback().accepted.generation.has_value());
	QCOMPARE(test.rig.state.fallback().accepted.generation->status, QStringLiteral("ready"));
	QTRY_COMPARE_WITH_TIMEOUT(test.rig.state.fallback().observation, FallbackObservation::Unknown, 1500);
	QTRY_VERIFY_WITH_TIMEOUT(!held->socket || held->socket->state() == QAbstractSocket::UnconnectedState, 1500);
	QCOMPARE(test.rig.state.fallback().acceptedAssetId, QStringLiteral("asset"));
	QCOMPARE(test.rig.state.fallback().acceptedGenerationId, QStringLiteral("new"));
	QCOMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Accepted);
	QCOMPARE(test.rig.state.fallback().accepted.generation->status, QStringLiteral("ready"));
	QCOMPARE(generationReads, 1);
	QCOMPARE(test.rig.mutations().size(), 1);
	if (holdFinalAsset)
		HttpServer::reply(*held, fixture::bytes(test.asset));
	else
		HttpServer::reply(*held, fixture::page({fallbackFixture::variant(QStringLiteral("new"),
										 QStringLiteral("second"))}));
	// sentinel подтверждает освобождение общего слота и обработку очереди после попытки позднего ответа.
	std::optional<Result<Asset>> sentinel;
	test.rig.configApi.getAsset(QStringLiteral("asset"), this, [&](auto, auto value) { sentinel = value; });
	QTRY_VERIFY_WITH_TIMEOUT(sentinel.has_value(), 1500);
	QVERIFY(*sentinel);
	QCOMPARE(test.rig.state.fallback().observation, FallbackObservation::Unknown);
	QCOMPARE(generationReads, 1);
	QCOMPARE(test.rig.mutations().size(), 1);
	// Новый явный цикл читает принятую пару без повторной подготовки и без смены серверного current.
	test.fallback.refresh();
	QTRY_COMPARE_WITH_TIMEOUT(test.rig.state.fallback().observation, FallbackObservation::Ready, 1500);
	QCOMPARE(test.rig.state.fallback().acceptedGenerationId, QStringLiteral("new"));
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	QCOMPARE(test.rig.state.fallback().asset->currentGenerationId, QStringLiteral("old"));
	QCOMPARE(test.rig.mutations().size(), 1);
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackControllerTest::policyUsesStreamRevisionAndPreservesDirtyName()
{
	// test начинает без политики, а форма Stream содержит несохранённое имя.
	FallbackRig test;
	test.policy = fallbackFixture::policy({});
	QVERIFY(test.initialize());
	QVERIFY(test.rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY(test.rig.state.configurationEditor().base.has_value());
	// draft — независимая копия формы с новым именем; её base revision не принадлежит policy.
	auto draft = std::get<Stream>(*test.rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Несохранённое имя");
	QVERIFY(test.rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	QVERIFY(test.fallback.setPolicyTarget(QStringLiteral("stream")));
	QTRY_VERIFY(test.rig.state.fallback().policy.base.has_value() && !test.rig.state.fallback().policy.loading);
	QVERIFY(test.fallback.setPolicyAsset(QStringLiteral("asset")));
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canSavePolicy(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.savePolicy(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.state.fallback().policy.outcome, FallbackWriteOutcome::Saved);
	QCOMPARE(test.rig.mutations().size(), 1);
	// initial проверяет согласованные интервалы первого назначения, без собственной формулы клиента.
	const auto initial = fixture::object(test.rig.mutations()[0].body);
	QCOMPARE(initial.value(QStringLiteral("fallback")),
		 fallbackFixture::policy().value(QStringLiteral("fallback")));
	QCOMPARE(initial.value(QStringLiteral("expected_revision")).toInteger(), qint64(1));
	QCOMPARE(std::get<Stream>(*test.rig.state.configurationEditor().draft).name,
		 QStringLiteral("Несохранённое имя"));
	QCOMPARE(std::get<Stream>(*test.rig.state.configurationEditor().base).revision, quint64(1));
	// replacement использует серверные значения, отличающиеся от defaults и от точности double.
	test.policy = fixture::object(
		R"({"revision":9007199254740993,"fallback":{"asset_id":"asset","watchdog":{"video_timeout_ns":9007199254740993,"audio_timeout_ns":9007199254740995,"recovery_window_ns":7,"keyframe_timeout_ns":5000000001}}})");
	test.fallback.refreshPolicy();
	QTRY_VERIFY(test.rig.state.fallback().policy.base &&
		    test.rig.state.fallback().policy.base->revision == 9007199254740993ULL);
	QVERIFY(test.fallback.setPolicyAsset(QStringLiteral("replacement")));
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canSavePolicy(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.savePolicy(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.mutations().size(), 2);
	QTRY_COMPARE(test.rig.state.fallback().policy.outcome, FallbackWriteOutcome::Saved);
	QVERIFY(test.rig.mutations()[1].body.contains("\"expected_revision\":9007199254740993"));
	QVERIFY(test.rig.mutations()[1].body.contains("\"video_timeout_ns\":9007199254740993"));
	QVERIFY(test.rig.mutations()[1].body.contains("\"audio_timeout_ns\":9007199254740995"));
	QVERIFY(test.rig.mutations()[1].body.contains("\"recovery_window_ns\":7"));
	QVERIFY(test.rig.mutations()[1].body.contains("\"keyframe_timeout_ns\":5000000001"));
	QVERIFY(test.fallback.setPolicyAsset({}));
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canSavePolicy(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.savePolicy(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.mutations().size(), 3);
	QCOMPARE(fixture::object(test.rig.mutations()[2].body).value(QStringLiteral("fallback")),
		 QJsonValue(QJsonValue::Null));
	QCOMPARE(test.rig.state.selectedStreamId(), QStringLiteral("stream"));
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackControllerTest::policyConflict_data()
{
	QTest::addColumn<QString>("code");
	QTest::newRow("cas") << QStringLiteral("revision_conflict");
	QTest::newRow("domain-rejection") << QStringLiteral("asset_in_use");
}

void FallbackControllerTest::policyConflict()
{
	// code определяет семантику 409 независимо от выросшей revision следующего GET.
	QFETCH(QString, code);
	FallbackRig test;
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.setPolicyTarget(QStringLiteral("stream")));
	QTRY_VERIFY(test.rig.state.fallback().policy.base.has_value() && !test.rig.state.fallback().policy.loading);
	QVERIFY(test.fallback.setPolicyAsset(QStringLiteral("replacement")));
	// Перехватчик возвращает выбранный код 409 и отдельную серверную базу, не повторяя намерение.
	test.intercept = [&](const auto &request) {
		if (request.method != "PUT")
			return false;
		test.policy = fallbackFixture::policy(QStringLiteral("server-choice"), 2);
		HttpServer::reply(
			request,
			fixture::bytes(QJsonObject{{QStringLiteral("status"), 409},
						   {QStringLiteral("code"), code},
						   {QStringLiteral("detail"), QStringLiteral("Изменение отклонено")}}),
			409, "application/problem+json");
		return true;
	};
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canSavePolicy(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.savePolicy(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_COMPARE(test.rig.state.fallback().policy.outcome, code == QStringLiteral("revision_conflict")
								       ? FallbackWriteOutcome::Conflict
								       : FallbackWriteOutcome::Rejected);
	// CAS-сравнение должно дождаться адресного ответа, а не только ранней публикации Conflict.
	if (code == QStringLiteral("revision_conflict"))
		QTRY_VERIFY(test.rig.state.fallback().policy.serverConflict.has_value());
	test.fallback.refreshPolicy();
	QTRY_VERIFY(!test.rig.state.fallback().policy.loading);
	QCOMPARE(test.rig.mutations().size(), 1);
	QCOMPARE(test.rig.state.fallback().policy.desiredAssetId, QStringLiteral("replacement"));
	QCOMPARE(test.fallback.confirmPolicyConflict(), code == QStringLiteral("revision_conflict"));
	QCOMPARE(test.rig.mutations().size(), 1);
}

void FallbackControllerTest::globalRefreshKeepsPendingPolicyComparison()
{
	// test начинает с подтверждённой policy revision1 и явным желаемым Asset.
	FallbackRig test;
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.setPolicyTarget(QStringLiteral("stream")));
	QTRY_VERIFY(test.rig.state.fallback().policy.base.has_value() && !test.rig.state.fallback().policy.loading);
	QVERIFY(test.fallback.setPolicyAsset(QStringLiteral("replacement")));
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canSavePolicy(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	// comparison удерживает первый GET после 409; rejected открывает перехват только после настоящего отказа PUT.
	std::optional<HttpServer::Request> comparison;
	bool rejected = false;
	// policyReads считает адресные чтения после 409, исключая первоначальную базу revision1.
	int policyReads = 0;
	// Перехватчик возвращает revision_conflict, затем удерживает сравнение до пользовательского Refresh.
	test.intercept = [&](const auto &request) {
		if (request.method == "PUT") {
			rejected = true;
			test.policy = fallbackFixture::policy(QStringLiteral("server-choice"), 2);
			HttpServer::reply(
				request,
				R"({"status":409,"code":"revision_conflict","detail":"Политика уже изменена"})", 409,
				"application/problem+json");
			return true;
		}
		if (rejected && request.method == "GET" && request.target == "/api/v1/streams/stream/fallback-policy") {
			++policyReads;
			if (!comparison) {
				comparison = request;
				return true;
			}
		}
		return false;
	};
	QVERIFY2(test.fallback.savePolicy(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_VERIFY(comparison.has_value());
	QCOMPARE(test.rig.state.fallback().policy.outcome, FallbackWriteOutcome::Conflict);
	QVERIFY(test.rig.state.fallback().policy.loading);
	QVERIFY(!test.rig.state.fallback().policy.serverConflict.has_value());
	// Обновление отменяет старый GET, но не отменяет подтверждённую необходимость CAS-сравнения.
	test.fallback.refresh();
	QTRY_VERIFY(test.rig.state.fallback().policy.serverConflict.has_value() &&
		    !test.rig.state.fallback().policy.loading);
	QCOMPARE(policyReads, 2);
	QCOMPARE(test.rig.state.fallback().policy.base->revision, quint64(1));
	QCOMPARE(test.rig.state.fallback().policy.serverConflict->revision, quint64(2));
	QVERIFY(test.rig.state.fallback().policy.serverConflict->fallback);
	QCOMPARE(test.rig.state.fallback().policy.serverConflict->fallback->assetId, QStringLiteral("server-choice"));
	QCOMPARE(test.rig.state.fallback().policy.desiredAssetId, QStringLiteral("replacement"));
	QCOMPARE(test.rig.mutations().size(), 1);
	QVERIFY(test.fallback.confirmPolicyConflict());
	QCOMPARE(test.rig.state.fallback().policy.base->revision, quint64(2));
	QCOMPARE(test.rig.state.fallback().policy.desiredAssetId, QStringLiteral("replacement"));
	QCOMPARE(test.rig.mutations().size(), 1);
}

void FallbackControllerTest::cancelledWritesRequireFreshReconciliation_data()
{
	QTest::addColumn<QString>("operation");
	// operation выбирает разные обязательства сверки: каталог, точный Asset либо policy.
	for (const auto *operation : {"create", "delete", "rebuild", "upload", "policy"})
		QTest::newRow(operation) << QString::fromLatin1(operation);
}

void FallbackControllerTest::cancelledWritesRequireFreshReconciliation()
{
	// operation задаёт намерение; test удерживает результат записи и все последующие чтения.
	QFETCH(QString, operation);
	FallbackRig test;
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	QVERIFY(test.fallback.setPolicyTarget(QStringLiteral("stream")));
	QTRY_VERIFY(test.rig.state.fallback().policy.base.has_value() && !test.rig.state.fallback().policy.loading);
	// file содержит синтетический оригинал только для upload; остальные операции его не читают.
	QFile file(test.rig.temp.filePath(QStringLiteral("source.bin")));
	QVERIFY(file.open(QIODevice::WriteOnly));
	QCOMPARE(file.write("source"), qint64(6));
	file.close();
	QVERIFY(test.fallback.setDraft(QStringLiteral("Новое имя"), file.fileName(), QStringLiteral("image")));
	QVERIFY(test.fallback.setPolicyAsset(QStringLiteral("replacement")));
	// writeArrived подтверждает dispatch; reconcile меняет только ответы последующих GET.
	bool writeArrived = false;
	bool reconcile = false;
	// Перехватчик принимает запись без ответа;503 после отмены не доказывает отсутствие изменения.
	test.intercept = [&](const auto &request) {
		if (request.method != "GET") {
			writeArrived = true;
			return true;
		}
		if (reconcile) {
			HttpServer::reply(request, "{}", 503);
			return true;
		}
		return false;
	};
	if (operation == QStringLiteral("create")) {
		QVERIFY(test.fallback.createAsset());
	} else if (operation == QStringLiteral("delete")) {
		QVERIFY(test.fallback.deleteAsset());
	} else if (operation == QStringLiteral("rebuild")) {
		QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)),
					  1500);
		QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	} else if (operation == QStringLiteral("upload")) {
		QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)),
					  1500);
		QVERIFY2(test.fallback.upload(), qPrintable(fallbackReadinessDiagnostic(test)));
	} else {
		QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canSavePolicy(), qPrintable(fallbackReadinessDiagnostic(test)),
					  1500);
		QVERIFY2(test.fallback.savePolicy(), qPrintable(fallbackReadinessDiagnostic(test)));
	}
	QTRY_VERIFY(writeArrived);
	reconcile = true;
	QVERIFY(test.fallback.cancel());
	QTRY_VERIFY(test.rig.state.fallback().reconciliationRequired);
	QVERIFY(!test.rig.state.configurationSettled(test.rig.state.serverOrigin()));
	QVERIFY(!test.fallback.canWrite());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("other")));
	test.fallback.refresh();
	QTRY_VERIFY(!test.rig.state.fallback().loading);
	QVERIFY(test.rig.state.fallback().reconciliationRequired);
	QVERIFY(!test.rig.operations.checkFallback(test.rig.state.dockContext()));
	QVERIFY(!test.rig.operations.startDestination(test.rig.state.dockContext(), QStringLiteral("destination")));
	QVERIFY(test.rig.configuration.beginCreate(ConfigurationKind::Stream, true));
	// streamDraft задаёт корректный ввод, чтобы отказ save зависел от общего Unknown, а не локальной валидации.
	auto streamDraft = std::get<Stream>(*test.rig.state.configurationEditor().draft);
	streamDraft.name = QStringLiteral("Другой поток");
	streamDraft.publicationKey = {FieldState::Value, QStringLiteral("key")};
	QVERIFY(test.rig.configuration.setDraft(streamDraft,
						{QStringLiteral("name"), QStringLiteral("publication_key")}));
	QVERIFY(!test.rig.configuration.save());
	QCOMPARE(test.rig.mutations().size(), 1);
	test.intercept = {};
	test.fallback.refresh();
	QTRY_VERIFY(!test.rig.state.fallback().reconciliationRequired);
	QVERIFY(test.rig.state.configurationSettled(test.rig.state.serverOrigin()));
	QCOMPARE(test.rig.mutations().size(), 1);
	if (operation == QStringLiteral("policy"))
		QCOMPARE(test.rig.state.fallback().policy.outcome, FallbackWriteOutcome::Unknown);
	else
		QCOMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Unknown);
}

void FallbackControllerTest::unknownCreateNeedsCompleteFreshCatalog()
{
	// apiLimits обрывает обход после одной страницы с next_cursor, сохраняя complete=false.
	Limits apiLimits;
	apiLimits.maxPages = 1;
	FallbackRig test({1000, 5}, apiLimits);
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.setDraft(QStringLiteral("Совпавшее имя"), {}, QStringLiteral("image")));
	// dispatched подтверждает отправку, partial включает неполный ответ только для сверки после отмены.
	bool dispatched = false;
	bool partial = false;
	// Перехватчик удерживает create и выдаёт неполную страницу только для последующей сверки Unknown.
	test.intercept = [&](const auto &request) {
		if (request.method == "POST") {
			dispatched = true;
			return true;
		}
		if (partial && request.target.startsWith("/api/v1/fallback-assets?")) {
			HttpServer::reply(request, fixture::page({test.asset}, QStringLiteral("more")));
			return true;
		}
		return false;
	};
	QVERIFY(test.fallback.createAsset());
	QTRY_VERIFY(dispatched);
	partial = true;
	QVERIFY(test.fallback.cancel());
	QTRY_VERIFY(test.rig.state.fallback().reconciliationRequired);
	test.fallback.refresh();
	QTRY_VERIFY(!test.rig.state.fallback().loading);
	QTRY_VERIFY(!test.rig.state.fallback().assetsStatus.complete &&
		    test.rig.state.fallback().assetsStatus.availability == RuntimeAvailability::Fresh);
	QVERIFY(test.rig.state.fallback().reconciliationRequired);
	QVERIFY(!test.fallback.createAsset());
	partial = false;
	test.fallback.refresh();
	QTRY_VERIFY(!test.rig.state.fallback().reconciliationRequired);
	QCOMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Unknown);
	QVERIFY(test.rig.state.fallback().acceptedGenerationId.isEmpty());
	QCOMPARE(test.rig.mutations().size(), 1);
}

void FallbackControllerTest::unknownPreparationKeepsUnownedPending()
{
	// test получает новое серверное pending без проверенного Preparation202.
	FallbackRig test({500, 5});
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	// dispatched подтверждает отправленный rebuild; последующие GET возвращают чужое выполняющееся pending.
	bool dispatched = false;
	// Перехватчик удерживает rebuild, чтобы последующий pending не имел подтверждённого собственного принятия.
	test.intercept = [&](const auto &request) {
		if (request.method != "POST")
			return false;
		dispatched = true;
		return true;
	};
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_VERIFY(dispatched);
	test.asset = fallbackFixture::asset(QStringLiteral("old"), QStringLiteral("foreign"));
	test.generations.insert(QStringLiteral("foreign"),
				fallbackFixture::generation(QStringLiteral("foreign"), QStringLiteral("preparing")));
	QVERIFY(test.fallback.cancel());
	QTRY_COMPARE(test.rig.state.fallback().pending.generationId, QStringLiteral("foreign"));
	QVERIFY(test.rig.state.fallback().reconciliationRequired);
	QVERIFY(test.rig.state.fallback().acceptedGenerationId.isEmpty());
	QVERIFY(!test.fallback.rebuild());
	QVERIFY(!test.fallback.upload());
	test.asset = fallbackFixture::asset(QStringLiteral("foreign"));
	test.generations.insert(QStringLiteral("foreign"),
				fallbackFixture::generation(QStringLiteral("foreign"), QStringLiteral("ready")));
	test.fallback.refresh();
	QTRY_VERIFY(!test.rig.state.fallback().reconciliationRequired);
	QVERIFY(test.rig.state.fallback().acceptedGenerationId.isEmpty());
	QCOMPARE(test.rig.state.fallback().outcome, FallbackWriteOutcome::Unknown);
	QCOMPARE(test.rig.mutations().size(), 1);
}

void FallbackControllerTest::configurationWriteBlocksFallbackAndBothPrechecks()
{
	// test открывает существующий Stream; heldWrite подтверждает отправку до конкурентных отказов.
	FallbackRig test;
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.setDraft(QStringLiteral("Новая заставка"), {}, QStringLiteral("image")));
	QVERIFY(test.rig.configuration.open(ConfigurationKind::Stream, QStringLiteral("stream")));
	QTRY_VERIFY(test.rig.state.configurationEditor().base.has_value());
	// draft содержит корректное новое имя, поэтому PATCH действительно отправляется.
	auto draft = std::get<Stream>(*test.rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Изменение");
	QVERIFY(test.rig.configuration.setDraft(draft, {QStringLiteral("name")}));
	// heldWrite отмечает уже отправленный PATCH, удерживающий общий барьер конфигурации.
	bool heldWrite = false;
	test.intercept = [&](const auto &request) {
		if (request.method != "PATCH")
			return false;
		heldWrite = true;
		return true;
	};
	QVERIFY(test.rig.configuration.save());
	QTRY_VERIFY(heldWrite);
	QVERIFY(!test.fallback.createAsset());
	QVERIFY(!test.fallback.checkReadiness());
	QVERIFY(!test.rig.operations.checkFallback(test.rig.state.dockContext()));
	QVERIFY(!test.rig.operations.startDestination(test.rig.state.dockContext(), QStringLiteral("destination")));
	QCOMPARE(test.rig.mutations().size(), 1);
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackControllerTest::activeAndPausedSessionForbidPreparation_data()
{
	QTest::addColumn<bool>("paused");
	QTest::newRow("running") << false;
	QTest::newRow("paused") << true;
}

void FallbackControllerTest::activeAndPausedSessionForbidPreparation()
{
	// paused меняет желаемую паузу одной и той же активной Session.
	QFETCH(bool, paused);
	FallbackRig test;
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	// session/dock задают авторитетную активную Session через существующий контракт Dock.
	const auto session = decode<Session>(fixture::bytes(fixture::session()));
	QVERIFY(session);
	auto dock = test.rig.state.dockServer();
	dock.session = DockSession{session.value->id, session.value->state, paused};
	dock.session->desiredPaused = paused;
	dock.sessionStatus = {RuntimeAvailability::Fresh, true, {}};
	test.rig.state.setDockServer(dock);
	QVERIFY(!test.fallback.canPrepare());
	QVERIFY(!test.fallback.rebuild());
	QVERIFY(!test.fallback.upload());
	QCOMPARE(test.rig.mutations().size(), 0);
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackControllerTest::contextChangeAndShutdownRejectLateAcceptance()
{
	// test удерживает отправленный rebuild; late содержит ровно исходное соединение старого профиля.
	FallbackRig test;
	QVERIFY(test.initialize());
	QVERIFY(test.fallback.selectAsset(QStringLiteral("asset")));
	QTRY_VERIFY(test.rig.state.fallback().assetFresh);
	std::optional<HttpServer::Request> late;
	// changing удерживает неуспешную сверку нового профиля до проверки старого 202.
	bool changing = false;
	// Перехватчик удерживает прежний rebuild и отклоняет GET нового профиля, не подтверждая старый 202.
	test.intercept = [&](const auto &request) {
		if (changing && request.method == "GET") {
			HttpServer::reply(request, "{}", 503);
			return true;
		}
		if (request.method == "POST" && request.target.endsWith("/rebuild")) {
			late = request;
			return true;
		}
		return false;
	};
	QTRY_VERIFY2_WITH_TIMEOUT(test.fallback.canPrepare(), qPrintable(fallbackReadinessDiagnostic(test)), 1500);
	QVERIFY2(test.fallback.rebuild(), qPrintable(fallbackReadinessDiagnostic(test)));
	QTRY_VERIFY(late.has_value());
	changing = true;
	test.rig.state.setSelection(QStringLiteral("stream"), true, {}, {}, QStringLiteral("other-profile"));
	HttpServer::reply(*late, R"({"asset_id":"asset","generation_id":"new"})", 202, "application/json",
			  "Location: /api/v1/fallback-assets/asset/generations/new\r\n");
	QCoreApplication::processEvents();
	QVERIFY(test.rig.state.fallback().acceptedGenerationId.isEmpty());
	QVERIFY(!test.rig.state.configurationSettled(test.rig.state.serverOrigin()));
	test.fallback.shutdown();
	test.fallback.shutdown();
	QVERIFY(!test.fallback.createAsset());
	QVERIFY(!test.fallback.rebuild());
	changing = false;
	// otherResult доказывает, что shutdown заёмщика не закрыл общий API.
	// Обработчик GET сохраняет полученный результат в otherResult.
	std::optional<Result<Asset>> otherResult;
	test.rig.configApi.getAsset(QStringLiteral("asset"), this, [&](auto, auto value) { otherResult = value; });
	QTRY_VERIFY(otherResult.has_value());
	QVERIFY(*otherResult);
	QCOMPARE(test.rig.mutations().size(), 1);
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

void FallbackControllerTest::contextChangeCancelsQueuedWriteBeforeFreeingSlot_data()
{
	QTest::addColumn<bool>("fallbackWriter");
	QTest::newRow("configuration-waits-for-fallback-read") << false;
	QTest::newRow("fallback-waits-for-configuration-read") << true;
}

void FallbackControllerTest::contextChangeCancelsQueuedWriteBeforeFreeingSlot()
{
	// fallbackWriter выбирает владельца ожидающего POST; второй контроллер удерживает слот чтением.
	QFETCH(bool, fallbackWriter);
	// limits запрещает параллельный dispatch, test использует настоящий общий configApi.
	Limits limits;
	limits.maxActiveRequests = 1;
	FallbackRig test({1000, 5}, limits);
	QVERIFY(test.initialize());
	QTRY_VERIFY(test.rig.state.fallback().assetsStatus.complete && !test.rig.state.fallback().loading);
	QVERIFY(test.fallback.setDraft(QStringLiteral("Новая заставка"), {}, QStringLiteral("image")));
	QVERIFY(test.rig.configuration.beginCreate(ConfigurationKind::Stream, true));
	// draft валиден без серверной базы; отказ должен зависеть только от отмены очереди.
	auto draft = std::get<Stream>(*test.rig.state.configurationEditor().draft);
	draft.name = QStringLiteral("Новый поток");
	draft.publicationKey = {FieldState::Value, QStringLiteral("publication")};
	QVERIFY(test.rig.configuration.setDraft(draft, {QStringLiteral("name"), QStringLiteral("publication_key")}));
	// held фиксирует настоящий GET до постановки записи; intercept не отвечает до смены контекста.
	std::optional<HttpServer::Request> held;
	// Перехватчик удерживает только чтение второго владельца и оставляет остальные маршруты обычной фикстуры.
	test.intercept = [&](const auto &request) {
		if (!held && request.method == "GET" &&
		    request.target.startsWith(fallbackWriter ? "/api/v1/transcode-profiles?"
							     : "/api/v1/fallback-assets?")) {
			held = request;
			return true;
		}
		return false;
	};
	// blockerStarted отмечает новый Fallback GET после штатной инвалидации, непосредственно перед send конфигурации.
	bool blockerStarted = false;
	// blockerReceiver снимает подписку до разрушения захваченных локальных значений и фикстуры.
	QObject blockerReceiver;
	if (fallbackWriter) {
		test.rig.configuration.refresh(ConfigurationKind::Profile);
		QTRY_VERIFY(held.has_value());
		QVERIFY(test.fallback.createAsset());
		QVERIFY(test.rig.state.fallback().writing);
	} else {
		// Штатный invalidateReads уже снял прежние GET; этот обработчик занимает слот новым чтением до POST.
		connect(&test.rig.state, &StateStore::configurationInvalidated, &blockerReceiver, [&] {
			if (!blockerStarted) {
				blockerStarted = true;
				test.fallback.refresh();
			}
		});
		QVERIFY(test.rig.configuration.save());
		QVERIFY(blockerStarted);
		QTRY_VERIFY(held.has_value());
		QVERIFY(test.rig.state.configurationEditor().writing);
	}
	QCOMPARE(test.rig.mutations().size(), 0);
	test.rig.state.setSelection(QStringLiteral("stream"), true, {}, {}, QStringLiteral("next-profile"));
	QCoreApplication::processEvents();
	QVERIFY(test.rig.mutations().isEmpty());
	QVERIFY(test.rig.state.configurationSettled(test.rig.state.serverOrigin()));
	QVERIFY(!test.rig.state.configurationEditor().requiresReconciliation());
	QVERIFY(test.rig.state.configurationEditor().outcome != ConfigurationOutcome::Unknown);
	QVERIFY(!test.rig.state.fallback().reconciliationRequired);
	QVERIFY(test.rig.state.fallback().outcome != FallbackWriteOutcome::Unknown);
	QCOMPARE(test.rig.frontend.stopRequests, 0);
}

QTEST_MAIN(FallbackControllerTest)
#include "fallback-controller-test.moc"
