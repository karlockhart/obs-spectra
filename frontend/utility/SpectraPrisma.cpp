#include "SpectraPrisma.hpp"

#include <obs.h>

namespace SpectraPrisma {

bool Ready()
{
	calldata_t cd = {0};
	bool ready = false;
	if (proc_handler_call(obs_get_proc_handler(), "spectra_prisma_ready", &cd)) {
		ready = calldata_bool(&cd, "ready");
	}
	calldata_free(&cd);
	return ready;
}

bool Export(const QString &kind, const QString &path, const QString &title, const QString &game,
	    const QDateTime &started, double duration, const QStringList &tags)
{
	const QByteArray k = kind.toUtf8(), p = path.toUtf8(), t = title.toUtf8(), g = game.toUtf8(),
			 s = started.isValid() ? started.toString(Qt::ISODate).toUtf8() : QByteArray(),
			 tagList = tags.join(',').toUtf8();
	calldata_t cd = {0};
	calldata_set_string(&cd, "kind", k.constData());
	calldata_set_string(&cd, "path", p.constData());
	calldata_set_string(&cd, "title", t.constData());
	calldata_set_string(&cd, "game", g.constData());
	calldata_set_string(&cd, "started", s.constData());
	calldata_set_float(&cd, "duration", duration);
	calldata_set_string(&cd, "tags", tagList.constData());
	const bool ok = proc_handler_call(obs_get_proc_handler(), "spectra_prisma_export", &cd);
	calldata_free(&cd);
	return ok;
}

} // namespace SpectraPrisma
