/****************************************************************************
 ** Makou Reactor Final Fantasy VII Field Script Editor
 ** Copyright (C) 2009-2022 Arzel Jérôme <myst6re@gmail.com>
 **
 ** This program is free software: you can redistribute it and/or modify
 ** it under the terms of the GNU General Public License as published by
 ** the Free Software Foundation, either version 3 of the License, or
 ** (at your option) any later version.
 **
 ** This program is distributed in the hope that it will be useful,
 ** but WITHOUT ANY WARRANTY; without even the implied warranty of
 ** MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 ** GNU General Public License for more details.
 **
 ** You should have received a copy of the GNU General Public License
 ** along with this program.  If not, see <http://www.gnu.org/licenses/>.
 ****************************************************************************/
#include "Section1File.h"
#include "Field.h"
#include "core/Config.h"
#include "core/FF7Font.h"
#include "FieldModelLoaderPC.h"
#include <limits>

Section1File::Section1File(Field *field) :
	FieldPart(field), _scale(0), _version(0)
{
}

namespace {
QString escapeScriptString(const QString &value)
{
	QString escaped = value;
	escaped.replace('\\', "\\\\");
	escaped.replace('"', "\\\"");
	escaped.replace('\t', "\\t");
	escaped.replace('\n', "\\n");
	escaped.replace('\r', "\\r");
	return escaped;
}

QString unescapeScriptString(const QString &value)
{
	QString unescaped;
	unescaped.reserve(value.size());
	for (int i = 0; i < value.size(); ++i) {
		const QChar ch = value.at(i);
		if (ch == '\\' && i + 1 < value.size()) {
			const QChar next = value.at(i + 1);
			switch (next.unicode()) {
			case 't':
				unescaped.append('\t');
				break;
			case 'n':
				unescaped.append('\n');
				break;
			case 'r':
				unescaped.append('\r');
				break;
			case '"':
				unescaped.append('"');
				break;
			case '\\':
				unescaped.append('\\');
				break;
			default:
				unescaped.append(next);
				break;
			}
			++i;
		} else {
			unescaped.append(ch);
		}
	}
	return unescaped;
}

QString formatHexValue(quint64 value, int width)
{
	return QString("0x%1").arg(value, width, 16, QLatin1Char('0')).toUpper();
}

QString opcodeOperatorString(quint8 oper)
{
	if (oper < OPERATORS_SIZE) {
		return QString("\"%1\"").arg(Opcode::operators[oper]);
	}
	return QString::number(oper);
}

void appendExecParams(QStringList &parts, quint8 groupID, quint8 scriptIDAndPriority)
{
	parts.append(QString("group=%1").arg(groupID));
	parts.append(QString("script=%1").arg(SCRIPT_ID(scriptIDAndPriority)));
	parts.append(QString("priority=%1").arg(PRIORITY(scriptIDAndPriority)));
}

void appendExecCharParams(QStringList &parts, quint8 partyID, quint8 scriptIDAndPriority)
{
	parts.append(QString("party=%1").arg(partyID));
	parts.append(QString("script=%1").arg(SCRIPT_ID(scriptIDAndPriority)));
	parts.append(QString("priority=%1").arg(PRIORITY(scriptIDAndPriority)));
}

void appendBanks(QStringList &parts, quint8 banks)
{
	parts.append(QString("bank1=%1").arg(B1(banks)));
	parts.append(QString("bank2=%1").arg(B2(banks)));
}

void appendBanks(QStringList &parts, const quint8 *banks, int count)
{
	for (int i = 0; i < count; ++i) {
		parts.append(QString("bank%1=%2").arg(i * 2 + 1).arg(B1(banks[i])));
		parts.append(QString("bank%1=%2").arg(i * 2 + 2).arg(B2(banks[i])));
	}
}

void appendJumpTarget(QStringList &parts, const Opcode &opcode)
{
	const int label = opcode.label();
	if (label >= 0) {
		parts.append(QString("target=%1").arg(label));
	}
}

bool parseNumber(const QString &value, qint64 &out)
{
	QString s = value.trimmed();
	if (s.isEmpty()) {
		return false;
	}
	bool negative = false;
	if (s.startsWith('-')) {
		negative = true;
		s = s.mid(1);
	}
	int base = 10;
	if (s.startsWith("0x", Qt::CaseInsensitive)) {
		base = 16;
		s = s.mid(2);
	}
	bool ok = false;
	qint64 parsed = s.toLongLong(&ok, base);
	if (!ok) {
		return false;
	}
	out = negative ? -parsed : parsed;
	return true;
}

bool parseKeyValueList(const QString &input, QMap<QString, QString> &out, QString *errorStr, int lineNo)
{
	int i = 0;
	const int n = input.size();
	while (i < n) {
		while (i < n && input.at(i).isSpace()) {
			++i;
		}
		if (i >= n) {
			break;
		}

		const int keyStart = i;
		while (i < n && !input.at(i).isSpace() && input.at(i) != '=') {
			++i;
		}
		if (i >= n || input.at(i) != '=') {
			if (errorStr) {
				*errorStr = QString("Line %1: expected key=value pair.").arg(lineNo);
			}
			return false;
		}

		const QString key = input.mid(keyStart, i - keyStart);
		++i; // skip '='

		if (i >= n) {
			if (errorStr) {
				*errorStr = QString("Line %1: missing value for '%2'.").arg(lineNo).arg(key);
			}
			return false;
		}

		QString value;
		if (input.at(i) == '"') {
			++i;
			bool closed = false;
			QString raw;
			for (; i < n; ++i) {
				const QChar ch = input.at(i);
				if (ch == '\\' && i + 1 < n) {
					raw.append(ch);
					raw.append(input.at(i + 1));
					++i;
					continue;
				}
				if (ch == '"') {
					closed = true;
					break;
				}
				raw.append(ch);
			}
			if (!closed) {
				if (errorStr) {
					*errorStr = QString("Line %1: unterminated quoted value for '%2'.").arg(lineNo).arg(key);
				}
				return false;
			}
			value = unescapeScriptString(raw);
			++i; // skip closing quote
		} else {
			const int valueStart = i;
			while (i < n && !input.at(i).isSpace()) {
				++i;
			}
			value = input.mid(valueStart, i - valueStart);
		}

		if (out.contains(key)) {
			if (errorStr) {
				*errorStr = QString("Line %1: duplicate key '%2'.").arg(lineNo).arg(key);
			}
			return false;
		}
		out.insert(key, value);
	}

	return true;
}

bool readParam(const QMap<QString, QString> &params, const QString &key, qint64 &out, QString *errorStr, int lineNo, bool required = true)
{
	if (!params.contains(key)) {
		if (required && errorStr) {
			*errorStr = QString("Line %1: missing parameter '%2'.").arg(lineNo).arg(key);
		}
		return !required;
	}
	if (!parseNumber(params.value(key), out)) {
		if (errorStr) {
			*errorStr = QString("Line %1: invalid number for '%2': %3").arg(lineNo).arg(key).arg(params.value(key));
		}
		return false;
	}
	return true;
}

template <typename T>
bool readParamRange(const QMap<QString, QString> &params, const QString &key, qint64 minValue, qint64 maxValue,
                    T &out, QString *errorStr, int lineNo, bool required = true)
{
	qint64 value = 0;
	if (!readParam(params, key, value, errorStr, lineNo, required)) {
		return !required;
	}
	if (value < minValue || value > maxValue) {
		if (errorStr) {
			*errorStr = QString("Line %1: value out of range for '%2': %3").arg(lineNo).arg(key).arg(value);
		}
		return false;
	}
	out = static_cast<T>(value);
	return true;
}

bool readOperator(const QMap<QString, QString> &params, const QString &key, quint8 &out, QString *errorStr, int lineNo)
{
	if (!params.contains(key)) {
		if (errorStr) {
			*errorStr = QString("Line %1: missing parameter '%2'.").arg(lineNo).arg(key);
		}
		return false;
	}
	const QString value = params.value(key).trimmed();
	for (int i = 0; i < OPERATORS_SIZE; ++i) {
		if (value == QLatin1String(Opcode::operators[i])) {
			out = quint8(i);
			return true;
		}
	}
	qint64 num = 0;
	if (!parseNumber(value, num)) {
		if (errorStr) {
			*errorStr = QString("Line %1: invalid operator value '%2'.").arg(lineNo).arg(value);
		}
		return false;
	}
	if (num < 0 || num > 0xFF) {
		if (errorStr) {
			*errorStr = QString("Line %1: operator value out of range '%2'.").arg(lineNo).arg(value);
		}
		return false;
	}
	out = quint8(num);
	return true;
}

bool readLabelTarget(const QMap<QString, QString> &params, quint16 &label, QString *errorStr, int lineNo)
{
	if (params.contains("target")) {
		return readParamRange(params, "target", 0, 0xFFFF, label, errorStr, lineNo);
	}
	if (params.contains("id")) {
		return readParamRange(params, "id", 0, 0xFFFF, label, errorStr, lineNo);
	}
	if (errorStr) {
		*errorStr = QString("Line %1: missing parameter 'target'.").arg(lineNo);
	}
	return false;
}

bool readBanks(const QMap<QString, QString> &params, int count, quint8 *banks, QString *errorStr, int lineNo)
{
	for (int i = 0; i < count; ++i) {
		const QString key1 = QString("bank%1").arg(i * 2 + 1);
		const QString key2 = QString("bank%1").arg(i * 2 + 2);
		quint8 b1 = 0;
		quint8 b2 = 0;
		if (!readParamRange(params, key1, 0, 0xF, b1, errorStr, lineNo)) {
			return false;
		}
		if (!readParamRange(params, key2, 0, 0xF, b2, errorStr, lineNo)) {
			return false;
		}
		banks[i] = BANK(b1, b2);
	}
	return true;
}

bool readHexParams(const QString &value, QByteArray &out, QString *errorStr, int lineNo)
{
	QString compact = value;
	compact.remove(' ');
	compact.remove('\t');
	compact.remove('\r');
	compact.remove('\n');
	if (compact.startsWith("0x", Qt::CaseInsensitive)) {
		compact = compact.mid(2);
	}
	if (compact.isEmpty()) {
		out.clear();
		return true;
	}
	if (compact.size() % 2 != 0) {
		if (errorStr) {
			*errorStr = QString("Line %1: params hex length must be even.").arg(lineNo);
		}
		return false;
	}
	const QByteArray bytes = QByteArray::fromHex(compact.toLatin1());
	if (bytes.isEmpty() && !compact.isEmpty()) {
		if (errorStr) {
			*errorStr = QString("Line %1: invalid hex params '%2'.").arg(lineNo).arg(value);
		}
		return false;
	}
	out = bytes;
	return true;
}

QString opcodeNamedParams(const Opcode &opcode)
{
	QStringList parts;
	auto join = [&parts]() {
		return parts.join(' ');
	};

	FF7BinaryOperation binOp;
	if (opcode.binaryOperation(binOp)) {
		parts.append(QString("bank1=%1").arg(binOp.bank1));
		parts.append(QString("bank2=%1").arg(binOp.bank2));
		parts.append(QString("var=%1").arg(binOp.var));
		parts.append(QString("value=%1").arg(binOp.value));
		return join();
	}

	FF7UnaryOperation unaryOp;
	if (opcode.unaryOperation(unaryOp)) {
		parts.append(QString("bank2=%1").arg(unaryOp.bank2));
		parts.append(QString("var=%1").arg(unaryOp.var));
		return join();
	}

	FF7BitOperation bitOp;
	if (opcode.bitOperation(bitOp)) {
		parts.append(QString("bank1=%1").arg(bitOp.bank1));
		parts.append(QString("bank2=%1").arg(bitOp.bank2));
		parts.append(QString("var=%1").arg(bitOp.var));
		parts.append(QString("position=%1").arg(bitOp.position));
		return join();
	}

	switch (opcode.id()) {
	case OpcodeKey::REQ:
		appendExecParams(parts, opcode.op().opcodeREQ.groupID, opcode.op().opcodeREQ.scriptIDAndPriority);
		return join();
	case OpcodeKey::REQSW:
		appendExecParams(parts, opcode.op().opcodeREQSW.groupID, opcode.op().opcodeREQSW.scriptIDAndPriority);
		return join();
	case OpcodeKey::REQEW:
		appendExecParams(parts, opcode.op().opcodeREQEW.groupID, opcode.op().opcodeREQEW.scriptIDAndPriority);
		return join();
	case OpcodeKey::PREQ:
		appendExecCharParams(parts, opcode.op().opcodePREQ.partyID, opcode.op().opcodePREQ.scriptIDAndPriority);
		return join();
	case OpcodeKey::PRQSW:
		appendExecCharParams(parts, opcode.op().opcodePRQSW.partyID, opcode.op().opcodePRQSW.scriptIDAndPriority);
		return join();
	case OpcodeKey::PRQEW:
		appendExecCharParams(parts, opcode.op().opcodePRQEW.partyID, opcode.op().opcodePRQEW.scriptIDAndPriority);
		return join();
	case OpcodeKey::RETTO:
		parts.append(QString("script=%1").arg(SCRIPT_ID(opcode.op().opcodeRETTO.scriptIDAndPriority)));
		parts.append(QString("priority=%1").arg(PRIORITY(opcode.op().opcodeRETTO.scriptIDAndPriority)));
		return join();
	case OpcodeKey::JOIN:
		parts.append(QString("speed=%1").arg(opcode.op().opcodeJOIN.speed));
		return join();
	case OpcodeKey::SPLIT: {
		const OpcodeSPLIT &op = opcode.op().opcodeSPLIT;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("targetX1=%1").arg(op.targetX1));
		parts.append(QString("targetY1=%1").arg(op.targetY1));
		parts.append(QString("direction1=%1").arg(op.direction1));
		parts.append(QString("targetX2=%1").arg(op.targetX2));
		parts.append(QString("targetY2=%1").arg(op.targetY2));
		parts.append(QString("direction2=%1").arg(op.direction2));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::SPTYE: {
		const OpcodeSPTYE &op = opcode.op().opcodeSPTYE;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("charID1=%1").arg(op.charID1));
		parts.append(QString("charID2=%1").arg(op.charID2));
		parts.append(QString("charID3=%1").arg(op.charID3));
		return join();
	}
	case OpcodeKey::GTPYE: {
		const OpcodeGTPYE &op = opcode.op().opcodeGTPYE;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("varCharID1=%1").arg(op.varCharID1));
		parts.append(QString("varCharID2=%1").arg(op.varCharID2));
		parts.append(QString("varCharID3=%1").arg(op.varCharID3));
		return join();
	}
	case OpcodeKey::DSKCG:
		parts.append(QString("diskID=%1").arg(opcode.op().opcodeDSKCG.diskID));
		return join();
	case OpcodeKey::SPECIAL: {
		const OpcodeSPECIAL &op = opcode.op().opcodeSPECIAL;
		parts.append(QString("subkey=%1").arg(formatHexValue(op.subKey, 2)));
		switch (OpcodeSpecialKey(op.subKey)) {
		case OpcodeSpecialKey::ARROW:
			parts.append(QString("disabled=%1").arg(opcode.op().opcodeSPECIALARROW.disabled));
			return join();
		case OpcodeSpecialKey::PNAME: {
			const OpcodeSPECIALPNAME &spec = opcode.op().opcodeSPECIALPNAME;
			appendBanks(parts, spec.banks);
			parts.append(QString("varOrValue=%1").arg(spec.varOrValue));
			parts.append(QString("unused=%1").arg(spec.unused));
			parts.append(QString("size=%1").arg(spec.size));
			return join();
		}
		case OpcodeSpecialKey::GMSPD: {
			const OpcodeSPECIALGMSPD &spec = opcode.op().opcodeSPECIALGMSPD;
			appendBanks(parts, spec.banks);
			parts.append(QString("varSpeed=%1").arg(spec.varSpeed));
			return join();
		}
		case OpcodeSpecialKey::SMSPD: {
			const OpcodeSPECIALSMSPD &spec = opcode.op().opcodeSPECIALSMSPD;
			appendBanks(parts, spec.banks);
			parts.append(QString("speed=%1").arg(spec.speed));
			return join();
		}
		case OpcodeSpecialKey::FLMAT:
		case OpcodeSpecialKey::FLITM:
		case OpcodeSpecialKey::RSGLB:
		case OpcodeSpecialKey::CLITM:
			return join();
		case OpcodeSpecialKey::BTLCK:
			parts.append(QString("lock=%1").arg(opcode.op().opcodeSPECIALBTLCK.lock));
			return join();
		case OpcodeSpecialKey::MVLCK:
			parts.append(QString("lock=%1").arg(opcode.op().opcodeSPECIALMVLCK.lock));
			return join();
		case OpcodeSpecialKey::SPCNM: {
			const OpcodeSPECIALSPCNM &spec = opcode.op().opcodeSPECIALSPCNM;
			parts.append(QString("charID=%1").arg(spec.charID));
			parts.append(QString("textID=%1").arg(spec.textID));
			return join();
		}
		}
		return join();
	}
	case OpcodeKey::JMPF:
	case OpcodeKey::JMPFL:
	case OpcodeKey::JMPB:
	case OpcodeKey::JMPBL:
	case OpcodeKey::Unused1B:
		appendJumpTarget(parts, opcode);
		return join();
	case OpcodeKey::IFUB: {
		const OpcodeIFUB &op = opcode.op().opcodeIFUB;
		parts.append(QString("bank1=%1").arg(B1(op.banks)));
		parts.append(QString("bank2=%1").arg(B2(op.banks)));
		parts.append(QString("left=%1").arg(op.value1));
		parts.append(QString("right=%1").arg(op.value2));
		parts.append(QString("op=%1").arg(opcodeOperatorString(op.oper)));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::IFUBL: {
		const OpcodeIFUBL &op = opcode.op().opcodeIFUBL;
		parts.append(QString("bank1=%1").arg(B1(op.banks)));
		parts.append(QString("bank2=%1").arg(B2(op.banks)));
		parts.append(QString("left=%1").arg(op.value1));
		parts.append(QString("right=%1").arg(op.value2));
		parts.append(QString("op=%1").arg(opcodeOperatorString(op.oper)));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::IFSW: {
		const OpcodeIFSW &op = opcode.op().opcodeIFSW;
		parts.append(QString("bank1=%1").arg(B1(op.banks)));
		parts.append(QString("bank2=%1").arg(B2(op.banks)));
		parts.append(QString("left=%1").arg(op.value1));
		parts.append(QString("right=%1").arg(op.value2));
		parts.append(QString("op=%1").arg(opcodeOperatorString(op.oper)));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::IFSWL: {
		const OpcodeIFSWL &op = opcode.op().opcodeIFSWL;
		parts.append(QString("bank1=%1").arg(B1(op.banks)));
		parts.append(QString("bank2=%1").arg(B2(op.banks)));
		parts.append(QString("left=%1").arg(op.value1));
		parts.append(QString("right=%1").arg(op.value2));
		parts.append(QString("op=%1").arg(opcodeOperatorString(op.oper)));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::IFUW: {
		const OpcodeIFUW &op = opcode.op().opcodeIFUW;
		parts.append(QString("bank1=%1").arg(B1(op.banks)));
		parts.append(QString("bank2=%1").arg(B2(op.banks)));
		parts.append(QString("left=%1").arg(op.value1));
		parts.append(QString("right=%1").arg(op.value2));
		parts.append(QString("op=%1").arg(opcodeOperatorString(op.oper)));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::IFUWL: {
		const OpcodeIFUWL &op = opcode.op().opcodeIFUWL;
		parts.append(QString("bank1=%1").arg(B1(op.banks)));
		parts.append(QString("bank2=%1").arg(B2(op.banks)));
		parts.append(QString("left=%1").arg(op.value1));
		parts.append(QString("right=%1").arg(op.value2));
		parts.append(QString("op=%1").arg(opcodeOperatorString(op.oper)));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::IFKEY:
	case OpcodeKey::IFKEYON:
	case OpcodeKey::IFKEYOFF: {
		const OpcodeIfKey &op = opcode.op().opcodeIFKEY;
		parts.append(QString("keys=%1").arg(formatHexValue(op.keys, 4)));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::IFPRTYQ:
	case OpcodeKey::IFMEMBQ: {
		const OpcodeIfQ &op = opcode.op().opcodeIFPRTYQ;
		parts.append(QString("charID=%1").arg(op.charID));
		appendJumpTarget(parts, opcode);
		return join();
	}
	case OpcodeKey::Unused1A: {
		const OpcodeUnused1A &op = opcode.op().opcodeUnused1A;
		parts.append(QString("from=%1").arg(op.from));
		parts.append(QString("to=%1").arg(op.to));
		parts.append(QString("absValue=%1").arg(op.absValue));
		parts.append(QString("flag=%1").arg(op.flag));
		return join();
	}
	case OpcodeKey::MINIGAME: {
		const OpcodeMINIGAME &op = opcode.op().opcodeMINIGAME;
		parts.append(QString("mapID=%1").arg(op.mapID));
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("targetI=%1").arg(op.targetI));
		parts.append(QString("minigameParam=%1").arg(op.minigameParam));
		parts.append(QString("minigameID=%1").arg(op.minigameID));
		return join();
	}
	case OpcodeKey::TUTOR:
		parts.append(QString("tutoID=%1").arg(opcode.op().opcodeTUTOR.tutoID));
		return join();
	case OpcodeKey::BTMD2:
		parts.append(QString("battleMode=%1").arg(opcode.op().opcodeBTMD2.battleMode));
		return join();
	case OpcodeKey::BTRLD: {
		const OpcodeBTRLD &op = opcode.op().opcodeBTRLD;
		appendBanks(parts, op.banks);
		parts.append(QString("var=%1").arg(op.var));
		return join();
	}
	case OpcodeKey::WAIT:
		parts.append(QString("frameCount=%1").arg(opcode.op().opcodeWAIT.frameCount));
		return join();
	case OpcodeKey::NFADE: {
		const OpcodeNFADE &op = opcode.op().opcodeNFADE;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("type=%1").arg(op.type));
		parts.append(QString("r=%1").arg(op.r));
		parts.append(QString("g=%1").arg(op.g));
		parts.append(QString("b=%1").arg(op.b));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::BLINK:
		parts.append(QString("closed=%1").arg(opcode.op().opcodeBLINK.closed));
		return join();
	case OpcodeKey::BGMOVIE:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeBGMOVIE.disabled));
		return join();
	case OpcodeKey::PMOVA:
		parts.append(QString("partyID=%1").arg(opcode.op().opcodePMOVA.partyID));
		return join();
	case OpcodeKey::SLIP:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeSLIP.disabled));
		return join();
	case OpcodeKey::BGPDH: {
		const OpcodeBGPDH &op = opcode.op().opcodeBGPDH;
		appendBanks(parts, op.banks);
		parts.append(QString("layerID=%1").arg(op.layerID));
		parts.append(QString("targetZ=%1").arg(op.targetZ));
		return join();
	}
	case OpcodeKey::BGSCR: {
		const OpcodeBGSCR &op = opcode.op().opcodeBGSCR;
		appendBanks(parts, op.banks);
		parts.append(QString("layerID=%1").arg(op.layerID));
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		return join();
	}
	case OpcodeKey::WCLS:
		parts.append(QString("windowID=%1").arg(opcode.op().opcodeWCLS.windowID));
		return join();
	case OpcodeKey::WSIZW: {
		const OpcodeWSIZW &op = opcode.op().opcodeWSIZW;
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("width=%1").arg(op.width));
		parts.append(QString("height=%1").arg(op.height));
		return join();
	}
	case OpcodeKey::UC:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeUC.disabled));
		return join();
	case OpcodeKey::PDIRA:
		parts.append(QString("partyID=%1").arg(opcode.op().opcodePDIRA.partyID));
		return join();
	case OpcodeKey::PTURA: {
		const OpcodePTURA &op = opcode.op().opcodePTURA;
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("directionRotation=%1").arg(op.directionRotation));
		return join();
	}
	case OpcodeKey::WSPCL: {
		const OpcodeWSPCL &op = opcode.op().opcodeWSPCL;
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("displayType=%1").arg(op.displayType));
		parts.append(QString("marginLeft=%1").arg(op.marginLeft));
		parts.append(QString("marginTop=%1").arg(op.marginTop));
		return join();
	}
	case OpcodeKey::WNUMB: {
		const OpcodeWNUMB &op = opcode.op().opcodeWNUMB;
		appendBanks(parts, op.banks);
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("value=%1").arg(op.value));
		parts.append(QString("digitCount=%1").arg(op.digitCount));
		return join();
	}
	case OpcodeKey::STTIM: {
		const OpcodeSTTIM &op = opcode.op().opcodeSTTIM;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("h=%1").arg(op.h));
		parts.append(QString("m=%1").arg(op.m));
		parts.append(QString("s=%1").arg(op.s));
		return join();
	}
	case OpcodeKey::GOLDu: {
		const OpcodeGOLDu &op = opcode.op().opcodeGOLDu;
		appendBanks(parts, op.banks);
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::GOLDd: {
		const OpcodeGOLDd &op = opcode.op().opcodeGOLDd;
		appendBanks(parts, op.banks);
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::CHGLD: {
		const OpcodeCHGLD &op = opcode.op().opcodeCHGLD;
		appendBanks(parts, op.banks);
		parts.append(QString("var1=%1").arg(op.var1));
		parts.append(QString("var2=%1").arg(op.var2));
		return join();
	}
	case OpcodeKey::MESSAGE: {
		const OpcodeMESSAGE &op = opcode.op().opcodeMESSAGE;
		parts.append(QString("text=%1").arg(op.textID));
		parts.append(QString("window=%1").arg(op.windowID));
		return join();
	}
	case OpcodeKey::MPNAM:
		parts.append(QString("text=%1").arg(opcode.op().opcodeMPNAM.textID));
		return join();
	case OpcodeKey::MPARA: {
		const OpcodeMPARA &op = opcode.op().opcodeMPARA;
		appendBanks(parts, op.banks);
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("windowVarID=%1").arg(op.windowVarID));
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::MPRA2: {
		const OpcodeMPRA2 &op = opcode.op().opcodeMPRA2;
		appendBanks(parts, op.banks);
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("windowVarID=%1").arg(op.windowVarID));
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::MPu: {
		const OpcodeMPu &op = opcode.op().opcodeMPu;
		appendBanks(parts, op.banks);
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::MPd: {
		const OpcodeMPd &op = opcode.op().opcodeMPd;
		appendBanks(parts, op.banks);
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::HPu: {
		const OpcodeHPu &op = opcode.op().opcodeHPu;
		appendBanks(parts, op.banks);
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::HPd: {
		const OpcodeHPd &op = opcode.op().opcodeHPd;
		appendBanks(parts, op.banks);
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("value=%1").arg(op.value));
		return join();
	}
	case OpcodeKey::ASK: {
		const OpcodeASK &op = opcode.op().opcodeASK;
		parts.append(QString("text=%1").arg(op.textID));
		parts.append(QString("window=%1").arg(op.windowID));
		parts.append(QString("first=%1").arg(op.firstLine));
		parts.append(QString("last=%1").arg(op.lastLine));
		parts.append(QString("answer=%1").arg(op.varAnswer));
		parts.append(QString("bank1=%1").arg(B1(op.banks)));
		parts.append(QString("bank2=%1").arg(B2(op.banks)));
		return join();
	}
	case OpcodeKey::MENU: {
		const OpcodeMENU &op = opcode.op().opcodeMENU;
		appendBanks(parts, op.banks);
		parts.append(QString("menuID=%1").arg(op.menuID));
		parts.append(QString("param=%1").arg(op.param));
		return join();
	}
	case OpcodeKey::MENU2:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeMENU2.disabled));
		return join();
	case OpcodeKey::BTLTB:
		parts.append(QString("battleTableID=%1").arg(opcode.op().opcodeBTLTB.battleTableID));
		return join();
	case OpcodeKey::WINDOW: {
		const OpcodeWINDOW &op = opcode.op().opcodeWINDOW;
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("width=%1").arg(op.width));
		parts.append(QString("height=%1").arg(op.height));
		return join();
	}
	case OpcodeKey::WMOVE: {
		const OpcodeWMOVE &op = opcode.op().opcodeWMOVE;
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("relativeX=%1").arg(op.relativeX));
		parts.append(QString("relativeY=%1").arg(op.relativeY));
		return join();
	}
	case OpcodeKey::WMODE: {
		const OpcodeWMODE &op = opcode.op().opcodeWMODE;
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("mode=%1").arg(op.mode));
		parts.append(QString("preventClose=%1").arg(op.preventClose));
		return join();
	}
	case OpcodeKey::WREST:
		parts.append(QString("windowID=%1").arg(opcode.op().opcodeWREST.windowID));
		return join();
	case OpcodeKey::WCLSE:
		parts.append(QString("windowID=%1").arg(opcode.op().opcodeWCLSE.windowID));
		return join();
	case OpcodeKey::WROW: {
		const OpcodeWROW &op = opcode.op().opcodeWROW;
		parts.append(QString("windowID=%1").arg(op.windowID));
		parts.append(QString("rowCount=%1").arg(op.rowCount));
		return join();
	}
	case OpcodeKey::GWCOL: {
		const OpcodeGWCOL &op = opcode.op().opcodeGWCOL;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("corner=%1").arg(op.corner));
		parts.append(QString("varR=%1").arg(op.varR));
		parts.append(QString("varG=%1").arg(op.varG));
		parts.append(QString("varB=%1").arg(op.varB));
		return join();
	}
	case OpcodeKey::SWCOL: {
		const OpcodeSWCOL &op = opcode.op().opcodeSWCOL;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("corner=%1").arg(op.corner));
		parts.append(QString("r=%1").arg(op.r));
		parts.append(QString("g=%1").arg(op.g));
		parts.append(QString("b=%1").arg(op.b));
		return join();
	}
	case OpcodeKey::STITM: {
		const OpcodeSTITM &op = opcode.op().opcodeSTITM;
		appendBanks(parts, op.banks);
		parts.append(QString("itemID=%1").arg(op.itemID));
		parts.append(QString("quantity=%1").arg(op.quantity));
		return join();
	}
	case OpcodeKey::DLITM: {
		const OpcodeDLITM &op = opcode.op().opcodeDLITM;
		appendBanks(parts, op.banks);
		parts.append(QString("itemID=%1").arg(op.itemID));
		parts.append(QString("quantity=%1").arg(op.quantity));
		return join();
	}
	case OpcodeKey::CKITM: {
		const OpcodeCKITM &op = opcode.op().opcodeCKITM;
		appendBanks(parts, op.banks);
		parts.append(QString("itemID=%1").arg(op.itemID));
		parts.append(QString("quantity=%1").arg(op.quantity));
		return join();
	}
	case OpcodeKey::SMTRA: {
		const OpcodeSMTRA &op = opcode.op().opcodeSMTRA;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("materiaID=%1").arg(op.materiaID));
		parts.append(QString("apCount1=%1").arg(op.APCount[0]));
		parts.append(QString("apCount2=%1").arg(op.APCount[1]));
		parts.append(QString("apCount3=%1").arg(op.APCount[2]));
		return join();
	}
	case OpcodeKey::DMTRA: {
		const OpcodeDMTRA &op = opcode.op().opcodeDMTRA;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("materiaID=%1").arg(op.materiaID));
		parts.append(QString("apCount1=%1").arg(op.APCount[0]));
		parts.append(QString("apCount2=%1").arg(op.APCount[1]));
		parts.append(QString("apCount3=%1").arg(op.APCount[2]));
		parts.append(QString("quantity=%1").arg(op.quantity));
		return join();
	}
	case OpcodeKey::CMTRA: {
		const OpcodeCMTRA &op = opcode.op().opcodeCMTRA;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("apCount1=%1").arg(op.APCount[0]));
		parts.append(QString("apCount2=%1").arg(op.APCount[1]));
		parts.append(QString("apCount3=%1").arg(op.APCount[2]));
		parts.append(QString("apCount4=%1").arg(op.APCount[3]));
		parts.append(QString("materiaID=%1").arg(op.materiaID));
		parts.append(QString("varQuantity=%1").arg(op.varQuantity));
		return join();
	}
	case OpcodeKey::SHAKE: {
		const OpcodeSHAKE &op = opcode.op().opcodeSHAKE;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("type=%1").arg(op.type));
		parts.append(QString("xAmplitude=%1").arg(op.xAmplitude));
		parts.append(QString("xFrames=%1").arg(op.xFrames));
		parts.append(QString("yAmplitude=%1").arg(op.yAmplitude));
		parts.append(QString("yFrames=%1").arg(op.yFrames));
		return join();
	}
	case OpcodeKey::MAPJUMP: {
		const OpcodeMAPJUMP &op = opcode.op().opcodeMAPJUMP;
		parts.append(QString("map=%1").arg(op.mapID));
		parts.append(QString("x=%1").arg(op.targetX));
		parts.append(QString("y=%1").arg(op.targetY));
		parts.append(QString("i=%1").arg(op.targetI));
		parts.append(QString("dir=%1").arg(op.direction));
		return join();
	}
	case OpcodeKey::SCRLO:
		parts.append(QString("unknown=%1").arg(opcode.op().opcodeSCRLO.unknown));
		return join();
	case OpcodeKey::SCRLC: {
		const OpcodeSCRLC &op = opcode.op().opcodeSCRLC;
		appendBanks(parts, op.banks);
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("unknown=%1").arg(op.unknown));
		return join();
	}
	case OpcodeKey::SCRLA: {
		const OpcodeSCRLA &op = opcode.op().opcodeSCRLA;
		appendBanks(parts, op.banks);
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("groupID=%1").arg(op.groupID));
		parts.append(QString("scrollType=%1").arg(op.scrollType));
		return join();
	}
	case OpcodeKey::SCR2D: {
		const OpcodeSCR2D &op = opcode.op().opcodeSCR2D;
		appendBanks(parts, op.banks);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		return join();
	}
	case OpcodeKey::SCR2DC: {
		const OpcodeSCR2DC &op = opcode.op().opcodeSCR2DC;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::SCR2DL: {
		const OpcodeSCR2DL &op = opcode.op().opcodeSCR2DL;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::MPDSP:
		parts.append(QString("unknown=%1").arg(opcode.op().opcodeMPDSP.unknown));
		return join();
	case OpcodeKey::VWOFT: {
		const OpcodeVWOFT &op = opcode.op().opcodeVWOFT;
		appendBanks(parts, op.banks);
		parts.append(QString("unknown1=%1").arg(op.unknown1));
		parts.append(QString("unknown2=%1").arg(op.unknown2));
		parts.append(QString("enable=%1").arg(op.enable));
		return join();
	}
	case OpcodeKey::FADE: {
		const OpcodeFADE &op = opcode.op().opcodeFADE;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("r=%1").arg(op.r));
		parts.append(QString("g=%1").arg(op.g));
		parts.append(QString("b=%1").arg(op.b));
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("fadeType=%1").arg(op.fadeType));
		parts.append(QString("adjust=%1").arg(op.adjust));
		return join();
	}
	case OpcodeKey::IDLCK: {
		const OpcodeIDLCK &op = opcode.op().opcodeIDLCK;
		parts.append(QString("triangleID=%1").arg(op.triangleID));
		parts.append(QString("locked=%1").arg(op.locked));
		return join();
	}
	case OpcodeKey::LSTMP: {
		const OpcodeLSTMP &op = opcode.op().opcodeLSTMP;
		appendBanks(parts, op.banks);
		parts.append(QString("var=%1").arg(op.var));
		return join();
	}
	case OpcodeKey::SCRLP: {
		const OpcodeSCRLP &op = opcode.op().opcodeSCRLP;
		appendBanks(parts, op.banks);
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("scrollType=%1").arg(op.scrollType));
		return join();
	}
	case OpcodeKey::BATTLE: {
		const OpcodeBATTLE &op = opcode.op().opcodeBATTLE;
		appendBanks(parts, op.banks);
		parts.append(QString("battleID=%1").arg(op.battleID));
		return join();
	}
	case OpcodeKey::BTLON:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeBTLON.disabled));
		return join();
	case OpcodeKey::BTLMD:
		parts.append(QString("battleMode=%1").arg(opcode.op().opcodeBTLMD.battleMode));
		return join();
	case OpcodeKey::PGTDR: {
		const OpcodePGTDR &op = opcode.op().opcodePGTDR;
		appendBanks(parts, op.banks);
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("varDir=%1").arg(op.varDir));
		return join();
	}
	case OpcodeKey::GETPC: {
		const OpcodeGETPC &op = opcode.op().opcodeGETPC;
		appendBanks(parts, op.banks);
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("varPC=%1").arg(op.varPC));
		return join();
	}
	case OpcodeKey::PXYZI: {
		const OpcodePXYZI &op = opcode.op().opcodePXYZI;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("partyID=%1").arg(op.partyID));
		parts.append(QString("varX=%1").arg(op.varX));
		parts.append(QString("varY=%1").arg(op.varY));
		parts.append(QString("varZ=%1").arg(op.varZ));
		parts.append(QString("varI=%1").arg(op.varI));
		return join();
	}
	case OpcodeKey::TOBYTE: {
		const OpcodeTOBYTE &op = opcode.op().opcodeTOBYTE;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("var=%1").arg(op.var));
		parts.append(QString("value1=%1").arg(op.value1));
		parts.append(QString("value2=%1").arg(op.value2));
		return join();
	}
	case OpcodeKey::SETX: {
		const OpcodeSETX &op = opcode.op().opcodeSETX;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("value=%1").arg(op.value));
		parts.append(QString("varOrValue1=%1").arg(op.varOrValue1));
		parts.append(QString("varOrValue2=%1").arg(op.varOrValue2));
		return join();
	}
	case OpcodeKey::GETX: {
		const OpcodeGETX &op = opcode.op().opcodeGETX;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("value=%1").arg(op.value));
		parts.append(QString("varOrValue1=%1").arg(op.varOrValue1));
		parts.append(QString("var=%1").arg(op.var));
		return join();
	}
	case OpcodeKey::SEARCHX: {
		const OpcodeSEARCHX &op = opcode.op().opcodeSEARCHX;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("searchStart=%1").arg(op.searchStart));
		parts.append(QString("start=%1").arg(op.start));
		parts.append(QString("end=%1").arg(op.end));
		parts.append(QString("value=%1").arg(op.value));
		parts.append(QString("varResult=%1").arg(op.varResult));
		return join();
	}
	case OpcodeKey::PC:
		parts.append(QString("charID=%1").arg(opcode.op().opcodePC.charID));
		return join();
	case OpcodeKey::CHAR_:
		parts.append(QString("object3DID=%1").arg(opcode.op().opcodeCHAR_.object3DID));
		return join();
	case OpcodeKey::DFANM: {
		const OpcodeDFANM &op = opcode.op().opcodeDFANM;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::ANIME1: {
		const OpcodeANIME1 &op = opcode.op().opcodeANIME1;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::VISI:
		parts.append(QString("show=%1").arg(opcode.op().opcodeVISI.show));
		return join();
	case OpcodeKey::XYZI: {
		const OpcodeXYZI &op = opcode.op().opcodeXYZI;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("targetZ=%1").arg(op.targetZ));
		parts.append(QString("targetI=%1").arg(op.targetI));
		return join();
	}
	case OpcodeKey::XYI: {
		const OpcodeXYI &op = opcode.op().opcodeXYI;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("targetI=%1").arg(op.targetI));
		return join();
	}
	case OpcodeKey::XYZ: {
		const OpcodeXYZ &op = opcode.op().opcodeXYZ;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("targetZ=%1").arg(op.targetZ));
		return join();
	}
	case OpcodeKey::MOVE: {
		const OpcodeMOVE &op = opcode.op().opcodeMOVE;
		appendBanks(parts, op.banks);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		return join();
	}
	case OpcodeKey::CMOVE: {
		const OpcodeCMOVE &op = opcode.op().opcodeCMOVE;
		appendBanks(parts, op.banks);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		return join();
	}
	case OpcodeKey::MOVA:
		parts.append(QString("groupID=%1").arg(opcode.op().opcodeMOVA.groupID));
		return join();
	case OpcodeKey::TURA: {
		const OpcodeTURA &op = opcode.op().opcodeTURA;
		parts.append(QString("groupID=%1").arg(op.groupID));
		parts.append(QString("directionRotation=%1").arg(op.directionRotation));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::FMOVE: {
		const OpcodeFMOVE &op = opcode.op().opcodeFMOVE;
		appendBanks(parts, op.banks);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		return join();
	}
	case OpcodeKey::ANIME2: {
		const OpcodeANIME2 &op = opcode.op().opcodeANIME2;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::ANIMX1: {
		const OpcodeANIMX1 &op = opcode.op().opcodeANIMX1;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::CANIM1: {
		const OpcodeCANIM1 &op = opcode.op().opcodeCANIM1;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("firstFrame=%1").arg(op.firstFrame));
		parts.append(QString("lastFrame=%1").arg(op.lastFrame));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::CANMX1: {
		const OpcodeCANMX1 &op = opcode.op().opcodeCANMX1;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("firstFrame=%1").arg(op.firstFrame));
		parts.append(QString("lastFrame=%1").arg(op.lastFrame));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::MSPED: {
		const OpcodeMSPED &op = opcode.op().opcodeMSPED;
		appendBanks(parts, op.banks);
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::DIR: {
		const OpcodeDIR &op = opcode.op().opcodeDIR;
		appendBanks(parts, op.banks);
		parts.append(QString("direction=%1").arg(op.direction));
		return join();
	}
	case OpcodeKey::TURNGEN: {
		const OpcodeTURNGEN &op = opcode.op().opcodeTURNGEN;
		appendBanks(parts, op.banks);
		parts.append(QString("direction=%1").arg(op.direction));
		parts.append(QString("turnCount=%1").arg(op.turnCount));
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("unknown=%1").arg(op.unknown));
		return join();
	}
	case OpcodeKey::TURN: {
		const OpcodeTURN &op = opcode.op().opcodeTURN;
		appendBanks(parts, op.banks);
		parts.append(QString("direction=%1").arg(op.direction));
		parts.append(QString("turnCount=%1").arg(op.turnCount));
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("unknown=%1").arg(op.unknown));
		return join();
	}
	case OpcodeKey::DIRA:
		parts.append(QString("groupID=%1").arg(opcode.op().opcodeDIRA.groupID));
		return join();
	case OpcodeKey::GETDIR: {
		const OpcodeGETDIR &op = opcode.op().opcodeGETDIR;
		appendBanks(parts, op.banks);
		parts.append(QString("groupID=%1").arg(op.groupID));
		parts.append(QString("varDir=%1").arg(op.varDir));
		return join();
	}
	case OpcodeKey::GETAXY: {
		const OpcodeGETAXY &op = opcode.op().opcodeGETAXY;
		appendBanks(parts, op.banks);
		parts.append(QString("groupID=%1").arg(op.groupID));
		parts.append(QString("varX=%1").arg(op.varX));
		parts.append(QString("varY=%1").arg(op.varY));
		return join();
	}
	case OpcodeKey::GETAI: {
		const OpcodeGETAI &op = opcode.op().opcodeGETAI;
		appendBanks(parts, op.banks);
		parts.append(QString("groupID=%1").arg(op.groupID));
		parts.append(QString("varI=%1").arg(op.varI));
		return join();
	}
	case OpcodeKey::ANIMX2: {
		const OpcodeANIMX2 &op = opcode.op().opcodeANIMX2;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::CANIM2: {
		const OpcodeCANIM2 &op = opcode.op().opcodeCANIM2;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("firstFrame=%1").arg(op.firstFrame));
		parts.append(QString("lastFrame=%1").arg(op.lastFrame));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::CANMX2: {
		const OpcodeCANMX2 &op = opcode.op().opcodeCANMX2;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("firstFrame=%1").arg(op.firstFrame));
		parts.append(QString("lastFrame=%1").arg(op.lastFrame));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::ASPED: {
		const OpcodeASPED &op = opcode.op().opcodeASPED;
		appendBanks(parts, op.banks);
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::CC:
		parts.append(QString("groupID=%1").arg(opcode.op().opcodeCC.groupID));
		return join();
	case OpcodeKey::JUMP: {
		const OpcodeJUMP &op = opcode.op().opcodeJUMP;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("targetI=%1").arg(op.targetI));
		parts.append(QString("height=%1").arg(op.height));
		return join();
	}
	case OpcodeKey::AXYZI: {
		const OpcodeAXYZI &op = opcode.op().opcodeAXYZI;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("groupID=%1").arg(op.groupID));
		parts.append(QString("varX=%1").arg(op.varX));
		parts.append(QString("varY=%1").arg(op.varY));
		parts.append(QString("varZ=%1").arg(op.varZ));
		parts.append(QString("varI=%1").arg(op.varI));
		return join();
	}
	case OpcodeKey::LADER: {
		const OpcodeLADER &op = opcode.op().opcodeLADER;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("targetZ=%1").arg(op.targetZ));
		parts.append(QString("targetI=%1").arg(op.targetI));
		parts.append(QString("way=%1").arg(op.way));
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("direction=%1").arg(op.direction));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::OFST: {
		const OpcodeOFST &op = opcode.op().opcodeOFST;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("moveType=%1").arg(op.moveType));
		parts.append(QString("targetX=%1").arg(op.targetX));
		parts.append(QString("targetY=%1").arg(op.targetY));
		parts.append(QString("targetZ=%1").arg(op.targetZ));
		parts.append(QString("speed=%1").arg(op.speed));
		return join();
	}
	case OpcodeKey::TALKR: {
		const OpcodeTALKR &op = opcode.op().opcodeTALKR;
		appendBanks(parts, op.banks);
		parts.append(QString("range=%1").arg(op.range));
		return join();
	}
	case OpcodeKey::SLIDR: {
		const OpcodeSLIDR &op = opcode.op().opcodeSLIDR;
		appendBanks(parts, op.banks);
		parts.append(QString("range=%1").arg(op.range));
		return join();
	}
	case OpcodeKey::SOLID:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeSOLID.disabled));
		return join();
	case OpcodeKey::PRTYP:
		parts.append(QString("charID=%1").arg(opcode.op().opcodePRTYP.charID));
		return join();
	case OpcodeKey::PRTYM:
		parts.append(QString("charID=%1").arg(opcode.op().opcodePRTYM.charID));
		return join();
	case OpcodeKey::PRTYE: {
		const OpcodePRTYE &op = opcode.op().opcodePRTYE;
		parts.append(QString("charID1=%1").arg(op.charID[0]));
		parts.append(QString("charID2=%1").arg(op.charID[1]));
		parts.append(QString("charID3=%1").arg(op.charID[2]));
		return join();
	}
	case OpcodeKey::MMBud: {
		const OpcodeMMBud &op = opcode.op().opcodeMMBud;
		parts.append(QString("exists=%1").arg(op.exists));
		parts.append(QString("charID=%1").arg(op.charID));
		return join();
	}
	case OpcodeKey::MMBLK:
		parts.append(QString("charID=%1").arg(opcode.op().opcodeMMBLK.charID));
		return join();
	case OpcodeKey::MMBUK:
		parts.append(QString("charID=%1").arg(opcode.op().opcodeMMBUK.charID));
		return join();
	case OpcodeKey::LINE: {
		const OpcodeLINE &op = opcode.op().opcodeLINE;
		parts.append(QString("targetX1=%1").arg(op.targetX1));
		parts.append(QString("targetY1=%1").arg(op.targetY1));
		parts.append(QString("targetZ1=%1").arg(op.targetZ1));
		parts.append(QString("targetX2=%1").arg(op.targetX2));
		parts.append(QString("targetY2=%1").arg(op.targetY2));
		parts.append(QString("targetZ2=%1").arg(op.targetZ2));
		return join();
	}
	case OpcodeKey::LINON:
		parts.append(QString("enabled=%1").arg(opcode.op().opcodeLINON.enabled));
		return join();
	case OpcodeKey::MPJPO:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeMPJPO.disabled));
		return join();
	case OpcodeKey::SLINE: {
		const OpcodeSLINE &op = opcode.op().opcodeSLINE;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("targetX1=%1").arg(op.targetX1));
		parts.append(QString("targetY1=%1").arg(op.targetY1));
		parts.append(QString("targetZ1=%1").arg(op.targetZ1));
		parts.append(QString("targetX2=%1").arg(op.targetX2));
		parts.append(QString("targetY2=%1").arg(op.targetY2));
		parts.append(QString("targetZ2=%1").arg(op.targetZ2));
		return join();
	}
	case OpcodeKey::SIN: {
		const OpcodeSIN &op = opcode.op().opcodeSIN;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("value1=%1").arg(op.value1));
		parts.append(QString("value2=%1").arg(op.value2));
		parts.append(QString("value3=%1").arg(op.value3));
		parts.append(QString("var=%1").arg(op.var));
		return join();
	}
	case OpcodeKey::COS: {
		const OpcodeCOS &op = opcode.op().opcodeCOS;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("value1=%1").arg(op.value1));
		parts.append(QString("value2=%1").arg(op.value2));
		parts.append(QString("value3=%1").arg(op.value3));
		parts.append(QString("var=%1").arg(op.var));
		return join();
	}
	case OpcodeKey::TLKR2: {
		const OpcodeTLKR2 &op = opcode.op().opcodeTLKR2;
		appendBanks(parts, op.banks);
		parts.append(QString("range=%1").arg(op.range));
		return join();
	}
	case OpcodeKey::SLDR2: {
		const OpcodeSLDR2 &op = opcode.op().opcodeSLDR2;
		appendBanks(parts, op.banks);
		parts.append(QString("range=%1").arg(op.range));
		return join();
	}
	case OpcodeKey::PMJMP:
		parts.append(QString("mapID=%1").arg(opcode.op().opcodePMJMP.mapID));
		return join();
	case OpcodeKey::AKAO2: {
		const OpcodeAKAO2 &op = opcode.op().opcodeAKAO2;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("opcode=%1").arg(op.opcode));
		parts.append(QString("param1=%1").arg(op.param1));
		parts.append(QString("param2=%1").arg(op.param2));
		parts.append(QString("param3=%1").arg(op.param3));
		parts.append(QString("param4=%1").arg(op.param4));
		parts.append(QString("param5=%1").arg(op.param5));
		return join();
	}
	case OpcodeKey::FCFIX:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeFCFIX.disabled));
		return join();
	case OpcodeKey::CCANM: {
		const OpcodeCCANM &op = opcode.op().opcodeCCANM;
		parts.append(QString("animID=%1").arg(op.animID));
		parts.append(QString("speed=%1").arg(op.speed));
		parts.append(QString("standWalkRun=%1").arg(op.standWalkRun));
		return join();
	}
	case OpcodeKey::MPPAL: {
		const OpcodeMPPAL &op = opcode.op().opcodeMPPAL;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("posDst=%1").arg(op.posDst));
		parts.append(QString("start=%1").arg(op.start));
		parts.append(QString("b=%1").arg(op.b));
		parts.append(QString("g=%1").arg(op.g));
		parts.append(QString("r=%1").arg(op.r));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::BGON: {
		const OpcodeBGON &op = opcode.op().opcodeBGON;
		appendBanks(parts, op.banks);
		parts.append(QString("bgParamID=%1").arg(op.bgParamID));
		parts.append(QString("bgStateID=%1").arg(op.bgStateID));
		return join();
	}
	case OpcodeKey::BGOFF: {
		const OpcodeBGOFF &op = opcode.op().opcodeBGOFF;
		appendBanks(parts, op.banks);
		parts.append(QString("bgParamID=%1").arg(op.bgParamID));
		parts.append(QString("bgStateID=%1").arg(op.bgStateID));
		return join();
	}
	case OpcodeKey::BGROL: {
		const OpcodeBGROL &op = opcode.op().opcodeBGROL;
		appendBanks(parts, op.banks);
		parts.append(QString("bgParamID=%1").arg(op.bgParamID));
		return join();
	}
	case OpcodeKey::BGROL2: {
		const OpcodeBGROL2 &op = opcode.op().opcodeBGROL2;
		appendBanks(parts, op.banks);
		parts.append(QString("bgParamID=%1").arg(op.bgParamID));
		return join();
	}
	case OpcodeKey::BGCLR: {
		const OpcodeBGCLR &op = opcode.op().opcodeBGCLR;
		appendBanks(parts, op.banks);
		parts.append(QString("bgParamID=%1").arg(op.bgParamID));
		return join();
	}
	case OpcodeKey::STPAL: {
		const OpcodeSTPAL &op = opcode.op().opcodeSTPAL;
		appendBanks(parts, op.banks);
		parts.append(QString("palID=%1").arg(op.palID));
		parts.append(QString("position=%1").arg(op.position));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::LDPAL: {
		const OpcodeLDPAL &op = opcode.op().opcodeLDPAL;
		appendBanks(parts, op.banks);
		parts.append(QString("position=%1").arg(op.position));
		parts.append(QString("palID=%1").arg(op.palID));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::CPPAL: {
		const OpcodeCPPAL &op = opcode.op().opcodeCPPAL;
		appendBanks(parts, op.banks);
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("posDst=%1").arg(op.posDst));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::RTPAL: {
		const OpcodeRTPAL &op = opcode.op().opcodeRTPAL;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("posDst=%1").arg(op.posDst));
		parts.append(QString("start=%1").arg(op.start));
		parts.append(QString("end=%1").arg(op.end));
		return join();
	}
	case OpcodeKey::ADPAL: {
		const OpcodeADPAL &op = opcode.op().opcodeADPAL;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("posDst=%1").arg(op.posDst));
		parts.append(QString("b=%1").arg(op.b));
		parts.append(QString("g=%1").arg(op.g));
		parts.append(QString("r=%1").arg(op.r));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::MPPAL2: {
		const OpcodeMPPAL2 &op = opcode.op().opcodeMPPAL2;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("posDst=%1").arg(op.posDst));
		parts.append(QString("b=%1").arg(op.b));
		parts.append(QString("g=%1").arg(op.g));
		parts.append(QString("r=%1").arg(op.r));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::STPLS: {
		const OpcodeSTPLS &op = opcode.op().opcodeSTPLS;
		parts.append(QString("palID=%1").arg(op.palID));
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("start=%1").arg(op.start));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::LDPLS: {
		const OpcodeLDPLS &op = opcode.op().opcodeLDPLS;
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("palID=%1").arg(op.palID));
		parts.append(QString("start=%1").arg(op.start));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::CPPAL2: {
		const OpcodeCPPAL2 &op = opcode.op().opcodeCPPAL2;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("posTileSrc=%1").arg(op.posTileSrc));
		parts.append(QString("posTileDst=%1").arg(op.posTileDst));
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("posDst=%1").arg(op.posDst));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::RTPAL2: {
		const OpcodeRTPAL2 &op = opcode.op().opcodeRTPAL2;
		appendBanks(parts, op.banks, 2);
		parts.append(QString("posTileSrc=%1").arg(op.posTileSrc));
		parts.append(QString("posTileDst=%1").arg(op.posTileDst));
		parts.append(QString("posSrc=%1").arg(op.posSrc));
		parts.append(QString("posDst=%1").arg(op.posDst));
		parts.append(QString("start=%1").arg(op.start));
		return join();
	}
	case OpcodeKey::ADPAL2: {
		const OpcodeADPAL2 &op = opcode.op().opcodeADPAL2;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("posTileSrc=%1").arg(op.posTileSrc));
		parts.append(QString("posTileDst=%1").arg(op.posTileDst));
		parts.append(QString("start=%1").arg(op.start));
		parts.append(QString("b=%1").arg(op.b));
		parts.append(QString("g=%1").arg(op.g));
		parts.append(QString("r=%1").arg(op.r));
		parts.append(QString("colorCount=%1").arg(op.colorCount));
		return join();
	}
	case OpcodeKey::MUSIC:
		parts.append(QString("musicID=%1").arg(opcode.op().opcodeMUSIC.musicID));
		return join();
	case OpcodeKey::SOUND: {
		const OpcodeSOUND &op = opcode.op().opcodeSOUND;
		appendBanks(parts, op.banks);
		parts.append(QString("soundID=%1").arg(op.soundID));
		parts.append(QString("position=%1").arg(op.position));
		return join();
	}
	case OpcodeKey::AKAO: {
		const OpcodeAKAO &op = opcode.op().opcodeAKAO;
		appendBanks(parts, op.banks, 3);
		parts.append(QString("opcode=%1").arg(op.opcode));
		parts.append(QString("param1=%1").arg(op.param1));
		parts.append(QString("param2=%1").arg(op.param2));
		parts.append(QString("param3=%1").arg(op.param3));
		parts.append(QString("param4=%1").arg(op.param4));
		parts.append(QString("param5=%1").arg(op.param5));
		return join();
	}
	case OpcodeKey::MUSVT:
		parts.append(QString("musicID=%1").arg(opcode.op().opcodeMUSVT.musicID));
		return join();
	case OpcodeKey::MUSVM:
		parts.append(QString("musicID=%1").arg(opcode.op().opcodeMUSVM.musicID));
		return join();
	case OpcodeKey::MULCK:
		parts.append(QString("disabled=%1").arg(opcode.op().opcodeMULCK.disabled));
		return join();
	case OpcodeKey::BMUSC:
		parts.append(QString("musicID=%1").arg(opcode.op().opcodeBMUSC.musicID));
		return join();
	case OpcodeKey::CHMPH: {
		const OpcodeCHMPH &op = opcode.op().opcodeCHMPH;
		appendBanks(parts, op.banks);
		parts.append(QString("var1=%1").arg(op.var1));
		parts.append(QString("var2=%1").arg(op.var2));
		return join();
	}
	case OpcodeKey::PMVIE:
		parts.append(QString("movieID=%1").arg(opcode.op().opcodePMVIE.movieID));
		return join();
	case OpcodeKey::MVIEF: {
		const OpcodeMVIEF &op = opcode.op().opcodeMVIEF;
		appendBanks(parts, op.banks);
		parts.append(QString("varCurMovieFrame=%1").arg(op.varCurMovieFrame));
		return join();
	}
	case OpcodeKey::MVCAM:
		parts.append(QString("movieCamID=%1").arg(opcode.op().opcodeMVCAM.movieCamID));
		return join();
	case OpcodeKey::FMUSC:
		parts.append(QString("musicID=%1").arg(opcode.op().opcodeFMUSC.musicID));
		return join();
	case OpcodeKey::CMUSC: {
		const OpcodeCMUSC &op = opcode.op().opcodeCMUSC;
		appendBanks(parts, op.banks);
		parts.append(QString("musicID=%1").arg(op.musicID));
		parts.append(QString("opcode=%1").arg(op.opcode));
		parts.append(QString("param1=%1").arg(op.param1));
		parts.append(QString("param2=%1").arg(op.param2));
		return join();
	}
	case OpcodeKey::CHMST: {
		const OpcodeCHMST &op = opcode.op().opcodeCHMST;
		appendBanks(parts, op.banks);
		parts.append(QString("var=%1").arg(op.var));
		return join();
	}
	default:
		break;
	}

	QByteArray params = opcode.params();
	if (params.isEmpty()) {
		return QString();
	}

	QString raw = QString::fromLatin1(params.toHex(' ')).toUpper();
	return QString("params=\"%1\"").arg(raw);
}

bool buildOpcodeFromParams(OpcodeKey key, const QMap<QString, QString> &params, Opcode &out, QString *errorStr, int lineNo)
{
	if (params.contains("params")) {
		if (params.size() != 1) {
			if (errorStr) {
				*errorStr = QString("Line %1: 'params' cannot be combined with named parameters.").arg(lineNo);
			}
			return false;
		}
		QByteArray raw;
		if (!readHexParams(params.value("params"), raw, errorStr, lineNo)) {
			return false;
		}
		out = Opcode(key, raw.constData(), raw.size());
		return true;
	}

	if (params.isEmpty() && key != OpcodeKey::SPECIAL && Opcode::length[int(key)] == 1) {
		out = Opcode(key, nullptr, 0);
		return true;
	}

	{
		Opcode op;
		op.op().id = key;
		FF7BinaryOperation bin;
		if (op.binaryOperation(bin)) {
			if (!readParamRange(params, "bank1", 0, 0xF, bin.bank1, errorStr, lineNo)) return false;
			if (!readParamRange(params, "bank2", 0, 0xF, bin.bank2, errorStr, lineNo)) return false;
			if (!readParamRange(params, "var", 0, 0xFF, bin.var, errorStr, lineNo)) return false;
			const qint64 maxValue = bin.isLong ? 0xFFFF : 0xFF;
			if (!readParamRange(params, "value", 0, maxValue, bin.value, errorStr, lineNo)) return false;
			op.setBinaryOperation(bin);
			out = op;
			return true;
		}
		FF7UnaryOperation unary;
		if (op.unaryOperation(unary)) {
			if (!readParamRange(params, "bank2", 0, 0xF, unary.bank2, errorStr, lineNo)) return false;
			if (!readParamRange(params, "var", 0, 0xFF, unary.var, errorStr, lineNo)) return false;
			op.setUnaryOperation(unary);
			out = op;
			return true;
		}
		FF7BitOperation bitOp;
		if (op.bitOperation(bitOp)) {
			if (!readParamRange(params, "bank1", 0, 0xF, bitOp.bank1, errorStr, lineNo)) return false;
			if (!readParamRange(params, "bank2", 0, 0xF, bitOp.bank2, errorStr, lineNo)) return false;
			if (!readParamRange(params, "var", 0, 0xFF, bitOp.var, errorStr, lineNo)) return false;
			if (!readParamRange(params, "position", 0, 0xFF, bitOp.position, errorStr, lineNo)) return false;
			op.setBitOperation(bitOp);
			out = op;
			return true;
		}
	}

	auto requireU8 = [&](const QString &name, quint8 &value) -> bool {
		return readParamRange(params, name, 0, 0xFF, value, errorStr, lineNo);
	};
	auto requireU16 = [&](const QString &name, quint16 &value) -> bool {
		return readParamRange(params, name, 0, 0xFFFF, value, errorStr, lineNo);
	};
	auto requireU32 = [&](const QString &name, quint32 &value) -> bool {
		return readParamRange(params, name, 0, 0xFFFFFFFFLL, value, errorStr, lineNo);
	};
	auto requireS16 = [&](const QString &name, qint16 &value) -> bool {
		return readParamRange(params, name, -0x8000, 0x7FFF, value, errorStr, lineNo);
	};
	auto requireS32 = [&](const QString &name, qint32 &value) -> bool {
		return readParamRange(params, name, std::numeric_limits<qint32>::min(), std::numeric_limits<qint32>::max(), value, errorStr, lineNo);
	};

	switch (key) {
	case OpcodeKey::REQ: {
		OpcodeREQ op = OpcodeREQ();
		if (!requireU8("group", op.groupID)) return false;
		quint8 script = 0;
		quint8 priority = 0;
		if (!readParamRange(params, "script", 0, 31, script, errorStr, lineNo)) return false;
		if (!readParamRange(params, "priority", 0, 7, priority, errorStr, lineNo)) return false;
		op.scriptIDAndPriority = SCRIPT_AND_PRIORITY(script, priority);
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::REQSW: {
		OpcodeREQSW op = OpcodeREQSW();
		if (!requireU8("group", op.groupID)) return false;
		quint8 script = 0;
		quint8 priority = 0;
		if (!readParamRange(params, "script", 0, 31, script, errorStr, lineNo)) return false;
		if (!readParamRange(params, "priority", 0, 7, priority, errorStr, lineNo)) return false;
		op.scriptIDAndPriority = SCRIPT_AND_PRIORITY(script, priority);
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::REQEW: {
		OpcodeREQEW op = OpcodeREQEW();
		if (!requireU8("group", op.groupID)) return false;
		quint8 script = 0;
		quint8 priority = 0;
		if (!readParamRange(params, "script", 0, 31, script, errorStr, lineNo)) return false;
		if (!readParamRange(params, "priority", 0, 7, priority, errorStr, lineNo)) return false;
		op.scriptIDAndPriority = SCRIPT_AND_PRIORITY(script, priority);
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PREQ: {
		OpcodePREQ op = OpcodePREQ();
		if (!requireU8("party", op.partyID)) return false;
		quint8 script = 0;
		quint8 priority = 0;
		if (!readParamRange(params, "script", 0, 31, script, errorStr, lineNo)) return false;
		if (!readParamRange(params, "priority", 0, 7, priority, errorStr, lineNo)) return false;
		op.scriptIDAndPriority = SCRIPT_AND_PRIORITY(script, priority);
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PRQSW: {
		OpcodePRQSW op = OpcodePRQSW();
		if (!requireU8("party", op.partyID)) return false;
		quint8 script = 0;
		quint8 priority = 0;
		if (!readParamRange(params, "script", 0, 31, script, errorStr, lineNo)) return false;
		if (!readParamRange(params, "priority", 0, 7, priority, errorStr, lineNo)) return false;
		op.scriptIDAndPriority = SCRIPT_AND_PRIORITY(script, priority);
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PRQEW: {
		OpcodePRQEW op = OpcodePRQEW();
		if (!requireU8("party", op.partyID)) return false;
		quint8 script = 0;
		quint8 priority = 0;
		if (!readParamRange(params, "script", 0, 31, script, errorStr, lineNo)) return false;
		if (!readParamRange(params, "priority", 0, 7, priority, errorStr, lineNo)) return false;
		op.scriptIDAndPriority = SCRIPT_AND_PRIORITY(script, priority);
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::RETTO: {
		OpcodeRETTO op = OpcodeRETTO();
		quint8 script = 0;
		quint8 priority = 0;
		if (!readParamRange(params, "script", 0, 31, script, errorStr, lineNo)) return false;
		if (!readParamRange(params, "priority", 0, 7, priority, errorStr, lineNo)) return false;
		op.scriptIDAndPriority = SCRIPT_AND_PRIORITY(script, priority);
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::JOIN: {
		OpcodeJOIN op = OpcodeJOIN();
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SPLIT: {
		OpcodeSPLIT op = OpcodeSPLIT();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX1", op.targetX1)) return false;
		if (!requireS16("targetY1", op.targetY1)) return false;
		if (!requireU8("direction1", op.direction1)) return false;
		if (!requireS16("targetX2", op.targetX2)) return false;
		if (!requireS16("targetY2", op.targetY2)) return false;
		if (!requireU8("direction2", op.direction2)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SPTYE: {
		OpcodeSPTYE op = OpcodeSPTYE();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("charID1", op.charID1)) return false;
		if (!requireU8("charID2", op.charID2)) return false;
		if (!requireU8("charID3", op.charID3)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GTPYE: {
		OpcodeGTPYE op = OpcodeGTPYE();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("varCharID1", op.varCharID1)) return false;
		if (!requireU8("varCharID2", op.varCharID2)) return false;
		if (!requireU8("varCharID3", op.varCharID3)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::DSKCG: {
		OpcodeDSKCG op = OpcodeDSKCG();
		if (!requireU8("diskID", op.diskID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SPECIAL: {
		quint8 subkey = 0;
		if (!readParamRange(params, "subkey", 0, 0xFF, subkey, errorStr, lineNo)) return false;
		switch (OpcodeSpecialKey(subkey)) {
		case OpcodeSpecialKey::ARROW: {
			OpcodeSPECIALARROW op = OpcodeSPECIALARROW();
			op.disabled = 0;
			if (params.contains("disabled") && !requireU8("disabled", op.disabled)) return false;
			out = Opcode(op);
			return true;
		}
		case OpcodeSpecialKey::PNAME: {
			OpcodeSPECIALPNAME op = OpcodeSPECIALPNAME();
			if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
			if (!requireU8("varOrValue", op.varOrValue)) return false;
			if (!requireU8("unused", op.unused)) return false;
			if (!requireU8("size", op.size)) return false;
			out = Opcode(op);
			return true;
		}
		case OpcodeSpecialKey::GMSPD: {
			OpcodeSPECIALGMSPD op = OpcodeSPECIALGMSPD();
			if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
			if (!requireU8("varSpeed", op.varSpeed)) return false;
			out = Opcode(op);
			return true;
		}
		case OpcodeSpecialKey::SMSPD: {
			OpcodeSPECIALSMSPD op = OpcodeSPECIALSMSPD();
			if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
			if (!requireU8("speed", op.speed)) return false;
			out = Opcode(op);
			return true;
		}
		case OpcodeSpecialKey::BTLCK: {
			OpcodeSPECIALBTLCK op = OpcodeSPECIALBTLCK();
			op.lock = 0;
			if (params.contains("lock") && !requireU8("lock", op.lock)) return false;
			out = Opcode(op);
			return true;
		}
		case OpcodeSpecialKey::MVLCK: {
			OpcodeSPECIALMVLCK op = OpcodeSPECIALMVLCK();
			op.lock = 0;
			if (params.contains("lock") && !requireU8("lock", op.lock)) return false;
			out = Opcode(op);
			return true;
		}
		case OpcodeSpecialKey::SPCNM: {
			OpcodeSPECIALSPCNM op = OpcodeSPECIALSPCNM();
			if (!requireU8("charID", op.charID)) return false;
			if (!requireU8("textID", op.textID)) return false;
			out = Opcode(op);
			return true;
		}
		case OpcodeSpecialKey::FLMAT:
		case OpcodeSpecialKey::FLITM:
		case OpcodeSpecialKey::RSGLB:
		case OpcodeSpecialKey::CLITM: {
			OpcodeSPECIAL op = OpcodeSPECIAL();
			op.subKey = subkey;
			out = Opcode(op);
			return true;
		}
		}
		if (errorStr) {
			*errorStr = QString("Line %1: unknown SPECIAL subkey 0x%2.").arg(lineNo).arg(subkey, 2, 16, QLatin1Char('0'));
		}
		return false;
	}
	case OpcodeKey::JMPF:
	case OpcodeKey::JMPFL:
	case OpcodeKey::JMPB:
	case OpcodeKey::JMPBL:
	case OpcodeKey::Unused1B: {
		Opcode op;
		op.op().id = key;
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		op.setLabel(label);
		out = op;
		return true;
	}
	case OpcodeKey::IFUB: {
		OpcodeIFUB op = OpcodeIFUB();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("left", op.value1)) return false;
		if (!requireU8("right", op.value2)) return false;
		if (!readOperator(params, "op", op.oper, errorStr, lineNo)) return false;
		out = Opcode(op);
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::IFUBL: {
		OpcodeIFUBL op = OpcodeIFUBL();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("left", op.value1)) return false;
		if (!requireU8("right", op.value2)) return false;
		if (!readOperator(params, "op", op.oper, errorStr, lineNo)) return false;
		out = Opcode(op);
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::IFSW: {
		OpcodeIFSW op = OpcodeIFSW();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS16("left", op.value1)) return false;
		if (!requireS16("right", op.value2)) return false;
		if (!readOperator(params, "op", op.oper, errorStr, lineNo)) return false;
		out = Opcode(op);
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::IFSWL: {
		OpcodeIFSWL op = OpcodeIFSWL();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS16("left", op.value1)) return false;
		if (!requireS16("right", op.value2)) return false;
		if (!readOperator(params, "op", op.oper, errorStr, lineNo)) return false;
		out = Opcode(op);
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::IFUW: {
		OpcodeIFUW op = OpcodeIFUW();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("left", op.value1)) return false;
		if (!requireU16("right", op.value2)) return false;
		if (!readOperator(params, "op", op.oper, errorStr, lineNo)) return false;
		out = Opcode(op);
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::IFUWL: {
		OpcodeIFUWL op = OpcodeIFUWL();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("left", op.value1)) return false;
		if (!requireU16("right", op.value2)) return false;
		if (!readOperator(params, "op", op.oper, errorStr, lineNo)) return false;
		out = Opcode(op);
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::IFKEY:
	case OpcodeKey::IFKEYON:
	case OpcodeKey::IFKEYOFF: {
		quint16 keys = 0;
		if (!readParamRange(params, "keys", 0, 0xFFFF, keys, errorStr, lineNo)) return false;
		if (key == OpcodeKey::IFKEY) {
			OpcodeIFKEY op = OpcodeIFKEY();
			op.keys = keys;
			out = Opcode(op);
		} else if (key == OpcodeKey::IFKEYON) {
			OpcodeIFKEYON op = OpcodeIFKEYON();
			op.keys = keys;
			out = Opcode(op);
		} else {
			OpcodeIFKEYOFF op = OpcodeIFKEYOFF();
			op.keys = keys;
			out = Opcode(op);
		}
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::IFPRTYQ:
	case OpcodeKey::IFMEMBQ: {
		quint8 charId = 0;
		if (!requireU8("charID", charId)) return false;
		if (key == OpcodeKey::IFPRTYQ) {
			OpcodeIFPRTYQ op = OpcodeIFPRTYQ();
			op.charID = charId;
			out = Opcode(op);
		} else {
			OpcodeIFMEMBQ op = OpcodeIFMEMBQ();
			op.charID = charId;
			out = Opcode(op);
		}
		quint16 label = 0;
		if (!readLabelTarget(params, label, errorStr, lineNo)) return false;
		out.setLabel(label);
		return true;
	}
	case OpcodeKey::Unused1A: {
		OpcodeUnused1A op = OpcodeUnused1A();
		if (!requireU16("from", op.from)) return false;
		if (!requireU16("to", op.to)) return false;
		if (!requireS32("absValue", op.absValue)) return false;
		if (!requireU8("flag", op.flag)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MINIGAME: {
		OpcodeMINIGAME op = OpcodeMINIGAME();
		if (!requireU16("mapID", op.mapID)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireU16("targetI", op.targetI)) return false;
		if (!requireU8("minigameParam", op.minigameParam)) return false;
		if (!requireU8("minigameID", op.minigameID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::TUTOR: {
		OpcodeTUTOR op = OpcodeTUTOR();
		if (!requireU8("tutoID", op.tutoID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BTMD2: {
		OpcodeBTMD2 op = OpcodeBTMD2();
		if (!requireU32("battleMode", op.battleMode)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BTRLD: {
		OpcodeBTRLD op = OpcodeBTRLD();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("var", op.var)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WAIT: {
		OpcodeWAIT op = OpcodeWAIT();
		if (!requireU16("frameCount", op.frameCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::NFADE: {
		OpcodeNFADE op = OpcodeNFADE();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("type", op.type)) return false;
		if (!requireU8("r", op.r)) return false;
		if (!requireU8("g", op.g)) return false;
		if (!requireU8("b", op.b)) return false;
		if (!requireU16("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BLINK: {
		OpcodeBLINK op = OpcodeBLINK();
		if (!requireU8("closed", op.closed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGMOVIE: {
		OpcodeBGMOVIE op = OpcodeBGMOVIE();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PMOVA: {
		OpcodePMOVA op = OpcodePMOVA();
		if (!requireU8("partyID", op.partyID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SLIP: {
		OpcodeSLIP op = OpcodeSLIP();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGPDH: {
		OpcodeBGPDH op = OpcodeBGPDH();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("layerID", op.layerID)) return false;
		if (!requireS16("targetZ", op.targetZ)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGSCR: {
		OpcodeBGSCR op = OpcodeBGSCR();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("layerID", op.layerID)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WCLS: {
		OpcodeWCLS op = OpcodeWCLS();
		if (!requireU8("windowID", op.windowID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WSIZW: {
		OpcodeWSIZW op = OpcodeWSIZW();
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireU16("width", op.width)) return false;
		if (!requireU16("height", op.height)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::UC: {
		OpcodeUC op = OpcodeUC();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PDIRA: {
		OpcodePDIRA op = OpcodePDIRA();
		if (!requireU8("partyID", op.partyID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PTURA: {
		OpcodePTURA op = OpcodePTURA();
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU8("speed", op.speed)) return false;
		if (!requireU8("directionRotation", op.directionRotation)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WSPCL: {
		OpcodeWSPCL op = OpcodeWSPCL();
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireU8("displayType", op.displayType)) return false;
		if (!requireU8("marginLeft", op.marginLeft)) return false;
		if (!requireU8("marginTop", op.marginTop)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WNUMB: {
		OpcodeWNUMB op = OpcodeWNUMB();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireS32("value", op.value)) return false;
		if (!requireU8("digitCount", op.digitCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::STTIM: {
		OpcodeSTTIM op = OpcodeSTTIM();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("h", op.h)) return false;
		if (!requireU8("m", op.m)) return false;
		if (!requireU8("s", op.s)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GOLDu: {
		OpcodeGOLDu op = OpcodeGOLDu();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS32("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GOLDd: {
		OpcodeGOLDd op = OpcodeGOLDd();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS32("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CHGLD: {
		OpcodeCHGLD op = OpcodeCHGLD();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("var1", op.var1)) return false;
		if (!requireU8("var2", op.var2)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MESSAGE: {
		OpcodeMESSAGE op = OpcodeMESSAGE();
		if (!requireU8("text", op.textID)) return false;
		if (!requireU8("window", op.windowID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPNAM: {
		OpcodeMPNAM op = OpcodeMPNAM();
		if (!requireU8("text", op.textID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPARA: {
		OpcodeMPARA op = OpcodeMPARA();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireU8("windowVarID", op.windowVarID)) return false;
		if (!requireU8("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPRA2: {
		OpcodeMPRA2 op = OpcodeMPRA2();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireU8("windowVarID", op.windowVarID)) return false;
		if (!requireU16("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPu: {
		OpcodeMPu op = OpcodeMPu();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU16("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPd: {
		OpcodeMPd op = OpcodeMPd();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU16("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::HPu: {
		OpcodeHPu op = OpcodeHPu();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU16("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::HPd: {
		OpcodeHPd op = OpcodeHPd();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU16("value", op.value)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ASK: {
		OpcodeASK op = OpcodeASK();
		if (!requireU8("text", op.textID)) return false;
		if (!requireU8("window", op.windowID)) return false;
		if (!requireU8("first", op.firstLine)) return false;
		if (!requireU8("last", op.lastLine)) return false;
		if (!requireU8("answer", op.varAnswer)) return false;
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MENU: {
		OpcodeMENU op = OpcodeMENU();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("menuID", op.menuID)) return false;
		if (!requireU8("param", op.param)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MENU2: {
		OpcodeMENU2 op = OpcodeMENU2();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BTLTB: {
		OpcodeBTLTB op = OpcodeBTLTB();
		if (!requireU8("battleTableID", op.battleTableID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WINDOW: {
		OpcodeWINDOW op = OpcodeWINDOW();
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireU16("width", op.width)) return false;
		if (!requireU16("height", op.height)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WMOVE: {
		OpcodeWMOVE op = OpcodeWMOVE();
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireS16("relativeX", op.relativeX)) return false;
		if (!requireS16("relativeY", op.relativeY)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WMODE: {
		OpcodeWMODE op = OpcodeWMODE();
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireU8("mode", op.mode)) return false;
		if (!requireU8("preventClose", op.preventClose)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WREST: {
		OpcodeWREST op = OpcodeWREST();
		if (!requireU8("windowID", op.windowID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WCLSE: {
		OpcodeWCLSE op = OpcodeWCLSE();
		if (!requireU8("windowID", op.windowID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::WROW: {
		OpcodeWROW op = OpcodeWROW();
		if (!requireU8("windowID", op.windowID)) return false;
		if (!requireU8("rowCount", op.rowCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GWCOL: {
		OpcodeGWCOL op = OpcodeGWCOL();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("corner", op.corner)) return false;
		if (!requireU8("varR", op.varR)) return false;
		if (!requireU8("varG", op.varG)) return false;
		if (!requireU8("varB", op.varB)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SWCOL: {
		OpcodeSWCOL op = OpcodeSWCOL();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("corner", op.corner)) return false;
		if (!requireU8("r", op.r)) return false;
		if (!requireU8("g", op.g)) return false;
		if (!requireU8("b", op.b)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::STITM: {
		OpcodeSTITM op = OpcodeSTITM();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("itemID", op.itemID)) return false;
		if (!requireU8("quantity", op.quantity)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::DLITM: {
		OpcodeDLITM op = OpcodeDLITM();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("itemID", op.itemID)) return false;
		if (!requireU8("quantity", op.quantity)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CKITM: {
		OpcodeCKITM op = OpcodeCKITM();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("itemID", op.itemID)) return false;
		if (!requireU8("quantity", op.quantity)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SMTRA: {
		OpcodeSMTRA op = OpcodeSMTRA();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("materiaID", op.materiaID)) return false;
		if (!requireU8("apCount1", op.APCount[0])) return false;
		if (!requireU8("apCount2", op.APCount[1])) return false;
		if (!requireU8("apCount3", op.APCount[2])) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::DMTRA: {
		OpcodeDMTRA op = OpcodeDMTRA();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("materiaID", op.materiaID)) return false;
		if (!requireU8("apCount1", op.APCount[0])) return false;
		if (!requireU8("apCount2", op.APCount[1])) return false;
		if (!requireU8("apCount3", op.APCount[2])) return false;
		if (!requireU8("quantity", op.quantity)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CMTRA: {
		OpcodeCMTRA op = OpcodeCMTRA();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("apCount1", op.APCount[0])) return false;
		if (!requireU8("apCount2", op.APCount[1])) return false;
		if (!requireU8("apCount3", op.APCount[2])) return false;
		if (!requireU8("apCount4", op.APCount[3])) return false;
		if (!requireU8("materiaID", op.materiaID)) return false;
		if (!requireU8("varQuantity", op.varQuantity)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SHAKE: {
		OpcodeSHAKE op = OpcodeSHAKE();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("type", op.type)) return false;
		if (!requireU8("xAmplitude", op.xAmplitude)) return false;
		if (!requireU8("xFrames", op.xFrames)) return false;
		if (!requireU8("yAmplitude", op.yAmplitude)) return false;
		if (!requireU8("yFrames", op.yFrames)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MAPJUMP: {
		OpcodeMAPJUMP op = OpcodeMAPJUMP();
		if (!requireU16("map", op.mapID)) return false;
		if (!requireS16("x", op.targetX)) return false;
		if (!requireS16("y", op.targetY)) return false;
		if (!requireU16("i", op.targetI)) return false;
		if (!requireU8("dir", op.direction)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SCRLO: {
		OpcodeSCRLO op = OpcodeSCRLO();
		if (!requireU8("unknown", op.unknown)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SCRLC: {
		OpcodeSCRLC op = OpcodeSCRLC();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("speed", op.speed)) return false;
		if (!requireU8("unknown", op.unknown)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SCRLA: {
		OpcodeSCRLA op = OpcodeSCRLA();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("speed", op.speed)) return false;
		if (!requireU8("groupID", op.groupID)) return false;
		if (!requireU8("scrollType", op.scrollType)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SCR2D: {
		OpcodeSCR2D op = OpcodeSCR2D();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SCR2DC: {
		OpcodeSCR2DC op = OpcodeSCR2DC();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireU16("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SCR2DL: {
		OpcodeSCR2DL op = OpcodeSCR2DL();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireU16("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPDSP: {
		OpcodeMPDSP op = OpcodeMPDSP();
		if (!requireU8("unknown", op.unknown)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::VWOFT: {
		OpcodeVWOFT op = OpcodeVWOFT();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("unknown1", op.unknown1)) return false;
		if (!requireU16("unknown2", op.unknown2)) return false;
		if (!requireU8("enable", op.enable)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::FADE: {
		OpcodeFADE op = OpcodeFADE();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("r", op.r)) return false;
		if (!requireU8("g", op.g)) return false;
		if (!requireU8("b", op.b)) return false;
		if (!requireU8("speed", op.speed)) return false;
		if (!requireU8("fadeType", op.fadeType)) return false;
		if (!requireU8("adjust", op.adjust)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::IDLCK: {
		OpcodeIDLCK op = OpcodeIDLCK();
		if (!requireU16("triangleID", op.triangleID)) return false;
		if (!requireU8("locked", op.locked)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::LSTMP: {
		OpcodeLSTMP op = OpcodeLSTMP();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("var", op.var)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SCRLP: {
		OpcodeSCRLP op = OpcodeSCRLP();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("speed", op.speed)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU8("scrollType", op.scrollType)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BATTLE: {
		OpcodeBATTLE op = OpcodeBATTLE();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("battleID", op.battleID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BTLON: {
		OpcodeBTLON op = OpcodeBTLON();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BTLMD: {
		OpcodeBTLMD op = OpcodeBTLMD();
		if (!requireU16("battleMode", op.battleMode)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PGTDR: {
		OpcodePGTDR op = OpcodePGTDR();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU8("varDir", op.varDir)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GETPC: {
		OpcodeGETPC op = OpcodeGETPC();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU8("varPC", op.varPC)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PXYZI: {
		OpcodePXYZI op = OpcodePXYZI();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("partyID", op.partyID)) return false;
		if (!requireU8("varX", op.varX)) return false;
		if (!requireU8("varY", op.varY)) return false;
		if (!requireU8("varZ", op.varZ)) return false;
		if (!requireU8("varI", op.varI)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::TOBYTE: {
		OpcodeTOBYTE op = OpcodeTOBYTE();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("var", op.var)) return false;
		if (!requireU8("value1", op.value1)) return false;
		if (!requireU8("value2", op.value2)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SETX: {
		OpcodeSETX op = OpcodeSETX();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("value", op.value)) return false;
		if (!requireU16("varOrValue1", op.varOrValue1)) return false;
		if (!requireU8("varOrValue2", op.varOrValue2)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GETX: {
		OpcodeGETX op = OpcodeGETX();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("value", op.value)) return false;
		if (!requireU16("varOrValue1", op.varOrValue1)) return false;
		if (!requireU8("var", op.var)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SEARCHX: {
		OpcodeSEARCHX op = OpcodeSEARCHX();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("searchStart", op.searchStart)) return false;
		if (!requireU16("start", op.start)) return false;
		if (!requireU16("end", op.end)) return false;
		if (!requireU8("value", op.value)) return false;
		if (!requireU8("varResult", op.varResult)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PC: {
		OpcodePC op = OpcodePC();
		if (!requireU8("charID", op.charID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CHAR_: {
		OpcodeCHAR_ op = OpcodeCHAR_();
		if (!requireU8("object3DID", op.object3DID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::DFANM: {
		OpcodeDFANM op = OpcodeDFANM();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ANIME1: {
		OpcodeANIME1 op = OpcodeANIME1();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::VISI: {
		OpcodeVISI op = OpcodeVISI();
		if (!requireU8("show", op.show)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::XYZI: {
		OpcodeXYZI op = OpcodeXYZI();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireS16("targetZ", op.targetZ)) return false;
		if (!requireU16("targetI", op.targetI)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::XYI: {
		OpcodeXYI op = OpcodeXYI();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireU16("targetI", op.targetI)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::XYZ: {
		OpcodeXYZ op = OpcodeXYZ();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireS16("targetZ", op.targetZ)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MOVE: {
		OpcodeMOVE op = OpcodeMOVE();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CMOVE: {
		OpcodeCMOVE op = OpcodeCMOVE();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MOVA: {
		OpcodeMOVA op = OpcodeMOVA();
		if (!requireU8("groupID", op.groupID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::TURA: {
		OpcodeTURA op = OpcodeTURA();
		if (!requireU8("groupID", op.groupID)) return false;
		if (!requireU8("directionRotation", op.directionRotation)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::FMOVE: {
		OpcodeFMOVE op = OpcodeFMOVE();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ANIME2: {
		OpcodeANIME2 op = OpcodeANIME2();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ANIMX1: {
		OpcodeANIMX1 op = OpcodeANIMX1();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CANIM1: {
		OpcodeCANIM1 op = OpcodeCANIM1();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("firstFrame", op.firstFrame)) return false;
		if (!requireU8("lastFrame", op.lastFrame)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CANMX1: {
		OpcodeCANMX1 op = OpcodeCANMX1();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("firstFrame", op.firstFrame)) return false;
		if (!requireU8("lastFrame", op.lastFrame)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MSPED: {
		OpcodeMSPED op = OpcodeMSPED();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::DIR: {
		OpcodeDIR op = OpcodeDIR();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("direction", op.direction)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::TURNGEN: {
		OpcodeTURNGEN op = OpcodeTURNGEN();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("direction", op.direction)) return false;
		if (!requireU8("turnCount", op.turnCount)) return false;
		if (!requireU8("speed", op.speed)) return false;
		if (!requireU8("unknown", op.unknown)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::TURN: {
		OpcodeTURN op = OpcodeTURN();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("direction", op.direction)) return false;
		if (!requireU8("turnCount", op.turnCount)) return false;
		if (!requireU8("speed", op.speed)) return false;
		if (!requireU8("unknown", op.unknown)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::DIRA: {
		OpcodeDIRA op = OpcodeDIRA();
		if (!requireU8("groupID", op.groupID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GETDIR: {
		OpcodeGETDIR op = OpcodeGETDIR();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("groupID", op.groupID)) return false;
		if (!requireU8("varDir", op.varDir)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GETAXY: {
		OpcodeGETAXY op = OpcodeGETAXY();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("groupID", op.groupID)) return false;
		if (!requireU8("varX", op.varX)) return false;
		if (!requireU8("varY", op.varY)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::GETAI: {
		OpcodeGETAI op = OpcodeGETAI();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("groupID", op.groupID)) return false;
		if (!requireU8("varI", op.varI)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ANIMX2: {
		OpcodeANIMX2 op = OpcodeANIMX2();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CANIM2: {
		OpcodeCANIM2 op = OpcodeCANIM2();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("firstFrame", op.firstFrame)) return false;
		if (!requireU8("lastFrame", op.lastFrame)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CANMX2: {
		OpcodeCANMX2 op = OpcodeCANMX2();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("firstFrame", op.firstFrame)) return false;
		if (!requireU8("lastFrame", op.lastFrame)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ASPED: {
		OpcodeASPED op = OpcodeASPED();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CC: {
		OpcodeCC op = OpcodeCC();
		if (!requireU8("groupID", op.groupID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::JUMP: {
		OpcodeJUMP op = OpcodeJUMP();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireU16("targetI", op.targetI)) return false;
		if (!requireS16("height", op.height)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::AXYZI: {
		OpcodeAXYZI op = OpcodeAXYZI();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("groupID", op.groupID)) return false;
		if (!requireU8("varX", op.varX)) return false;
		if (!requireU8("varY", op.varY)) return false;
		if (!requireU8("varZ", op.varZ)) return false;
		if (!requireU8("varI", op.varI)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::LADER: {
		OpcodeLADER op = OpcodeLADER();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireS16("targetZ", op.targetZ)) return false;
		if (!requireU16("targetI", op.targetI)) return false;
		if (!requireU8("way", op.way)) return false;
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("direction", op.direction)) return false;
		if (!requireU8("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::OFST: {
		OpcodeOFST op = OpcodeOFST();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("moveType", op.moveType)) return false;
		if (!requireS16("targetX", op.targetX)) return false;
		if (!requireS16("targetY", op.targetY)) return false;
		if (!requireS16("targetZ", op.targetZ)) return false;
		if (!requireU16("speed", op.speed)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::TALKR: {
		OpcodeTALKR op = OpcodeTALKR();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("range", op.range)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SLIDR: {
		OpcodeSLIDR op = OpcodeSLIDR();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("range", op.range)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SOLID: {
		OpcodeSOLID op = OpcodeSOLID();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PRTYP: {
		OpcodePRTYP op = OpcodePRTYP();
		if (!requireU8("charID", op.charID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PRTYM: {
		OpcodePRTYM op = OpcodePRTYM();
		if (!requireU8("charID", op.charID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PRTYE: {
		OpcodePRTYE op = OpcodePRTYE();
		if (!requireU8("charID1", op.charID[0])) return false;
		if (!requireU8("charID2", op.charID[1])) return false;
		if (!requireU8("charID3", op.charID[2])) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MMBud: {
		OpcodeMMBud op = OpcodeMMBud();
		if (!requireU8("exists", op.exists)) return false;
		if (!requireU8("charID", op.charID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MMBLK: {
		OpcodeMMBLK op = OpcodeMMBLK();
		if (!requireU8("charID", op.charID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MMBUK: {
		OpcodeMMBUK op = OpcodeMMBUK();
		if (!requireU8("charID", op.charID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::LINE: {
		OpcodeLINE op = OpcodeLINE();
		if (!requireS16("targetX1", op.targetX1)) return false;
		if (!requireS16("targetY1", op.targetY1)) return false;
		if (!requireS16("targetZ1", op.targetZ1)) return false;
		if (!requireS16("targetX2", op.targetX2)) return false;
		if (!requireS16("targetY2", op.targetY2)) return false;
		if (!requireS16("targetZ2", op.targetZ2)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::LINON: {
		OpcodeLINON op = OpcodeLINON();
		if (!requireU8("enabled", op.enabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPJPO: {
		OpcodeMPJPO op = OpcodeMPJPO();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SLINE: {
		OpcodeSLINE op = OpcodeSLINE();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("targetX1", op.targetX1)) return false;
		if (!requireS16("targetY1", op.targetY1)) return false;
		if (!requireS16("targetZ1", op.targetZ1)) return false;
		if (!requireS16("targetX2", op.targetX2)) return false;
		if (!requireS16("targetY2", op.targetY2)) return false;
		if (!requireS16("targetZ2", op.targetZ2)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SIN: {
		OpcodeSIN op = OpcodeSIN();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("value1", op.value1)) return false;
		if (!requireS16("value2", op.value2)) return false;
		if (!requireS16("value3", op.value3)) return false;
		if (!requireU8("var", op.var)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::COS: {
		OpcodeCOS op = OpcodeCOS();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireS16("value1", op.value1)) return false;
		if (!requireS16("value2", op.value2)) return false;
		if (!requireS16("value3", op.value3)) return false;
		if (!requireU8("var", op.var)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::TLKR2: {
		OpcodeTLKR2 op = OpcodeTLKR2();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("range", op.range)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SLDR2: {
		OpcodeSLDR2 op = OpcodeSLDR2();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("range", op.range)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PMJMP: {
		OpcodePMJMP op = OpcodePMJMP();
		if (!requireU16("mapID", op.mapID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::AKAO2: {
		OpcodeAKAO2 op = OpcodeAKAO2();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("opcode", op.opcode)) return false;
		if (!requireU16("param1", op.param1)) return false;
		if (!requireU16("param2", op.param2)) return false;
		if (!requireU16("param3", op.param3)) return false;
		if (!requireU16("param4", op.param4)) return false;
		if (!requireU16("param5", op.param5)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::FCFIX: {
		OpcodeFCFIX op = OpcodeFCFIX();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CCANM: {
		OpcodeCCANM op = OpcodeCCANM();
		if (!requireU8("animID", op.animID)) return false;
		if (!requireU8("speed", op.speed)) return false;
		if (!requireU8("standWalkRun", op.standWalkRun)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPPAL: {
		OpcodeMPPAL op = OpcodeMPPAL();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("posDst", op.posDst)) return false;
		if (!requireU8("start", op.start)) return false;
		if (!requireU8("b", op.b)) return false;
		if (!requireU8("g", op.g)) return false;
		if (!requireU8("r", op.r)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGON: {
		OpcodeBGON op = OpcodeBGON();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("bgParamID", op.bgParamID)) return false;
		if (!requireU8("bgStateID", op.bgStateID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGOFF: {
		OpcodeBGOFF op = OpcodeBGOFF();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("bgParamID", op.bgParamID)) return false;
		if (!requireU8("bgStateID", op.bgStateID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGROL: {
		OpcodeBGROL op = OpcodeBGROL();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("bgParamID", op.bgParamID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGROL2: {
		OpcodeBGROL2 op = OpcodeBGROL2();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("bgParamID", op.bgParamID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BGCLR: {
		OpcodeBGCLR op = OpcodeBGCLR();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("bgParamID", op.bgParamID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::STPAL: {
		OpcodeSTPAL op = OpcodeSTPAL();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("palID", op.palID)) return false;
		if (!requireU8("position", op.position)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::LDPAL: {
		OpcodeLDPAL op = OpcodeLDPAL();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("position", op.position)) return false;
		if (!requireU8("palID", op.palID)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CPPAL: {
		OpcodeCPPAL op = OpcodeCPPAL();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("posDst", op.posDst)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::RTPAL: {
		OpcodeRTPAL op = OpcodeRTPAL();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("posDst", op.posDst)) return false;
		if (!requireU8("start", op.start)) return false;
		if (!requireU8("end", op.end)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ADPAL: {
		OpcodeADPAL op = OpcodeADPAL();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("posDst", op.posDst)) return false;
		if (!requireU8("b", op.b)) return false;
		if (!requireU8("g", op.g)) return false;
		if (!requireU8("r", op.r)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MPPAL2: {
		OpcodeMPPAL2 op = OpcodeMPPAL2();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("posDst", op.posDst)) return false;
		if (!requireU8("b", op.b)) return false;
		if (!requireU8("g", op.g)) return false;
		if (!requireU8("r", op.r)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::STPLS: {
		OpcodeSTPLS op = OpcodeSTPLS();
		if (!requireU8("palID", op.palID)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("start", op.start)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::LDPLS: {
		OpcodeLDPLS op = OpcodeLDPLS();
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("palID", op.palID)) return false;
		if (!requireU8("start", op.start)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CPPAL2: {
		OpcodeCPPAL2 op = OpcodeCPPAL2();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posTileSrc", op.posTileSrc)) return false;
		if (!requireU8("posTileDst", op.posTileDst)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("posDst", op.posDst)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::RTPAL2: {
		OpcodeRTPAL2 op = OpcodeRTPAL2();
		if (!readBanks(params, 2, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posTileSrc", op.posTileSrc)) return false;
		if (!requireU8("posTileDst", op.posTileDst)) return false;
		if (!requireU8("posSrc", op.posSrc)) return false;
		if (!requireU8("posDst", op.posDst)) return false;
		if (!requireU8("start", op.start)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::ADPAL2: {
		OpcodeADPAL2 op = OpcodeADPAL2();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("posTileSrc", op.posTileSrc)) return false;
		if (!requireU8("posTileDst", op.posTileDst)) return false;
		if (!requireU8("start", op.start)) return false;
		if (!requireU8("b", op.b)) return false;
		if (!requireU8("g", op.g)) return false;
		if (!requireU8("r", op.r)) return false;
		if (!requireU8("colorCount", op.colorCount)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MUSIC: {
		OpcodeMUSIC op = OpcodeMUSIC();
		if (!requireU8("musicID", op.musicID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::SOUND: {
		OpcodeSOUND op = OpcodeSOUND();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU16("soundID", op.soundID)) return false;
		if (!requireU8("position", op.position)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::AKAO: {
		OpcodeAKAO op = OpcodeAKAO();
		if (!readBanks(params, 3, op.banks, errorStr, lineNo)) return false;
		if (!requireU8("opcode", op.opcode)) return false;
		if (!requireU8("param1", op.param1)) return false;
		if (!requireU16("param2", op.param2)) return false;
		if (!requireU16("param3", op.param3)) return false;
		if (!requireU16("param4", op.param4)) return false;
		if (!requireU16("param5", op.param5)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MUSVT: {
		OpcodeMUSVT op = OpcodeMUSVT();
		if (!requireU8("musicID", op.musicID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MUSVM: {
		OpcodeMUSVM op = OpcodeMUSVM();
		if (!requireU8("musicID", op.musicID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MULCK: {
		OpcodeMULCK op = OpcodeMULCK();
		if (!requireU8("disabled", op.disabled)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::BMUSC: {
		OpcodeBMUSC op = OpcodeBMUSC();
		if (!requireU8("musicID", op.musicID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CHMPH: {
		OpcodeCHMPH op = OpcodeCHMPH();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("var1", op.var1)) return false;
		if (!requireU8("var2", op.var2)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::PMVIE: {
		OpcodePMVIE op = OpcodePMVIE();
		if (!requireU8("movieID", op.movieID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MVIEF: {
		OpcodeMVIEF op = OpcodeMVIEF();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("varCurMovieFrame", op.varCurMovieFrame)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::MVCAM: {
		OpcodeMVCAM op = OpcodeMVCAM();
		if (!requireU8("movieCamID", op.movieCamID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::FMUSC: {
		OpcodeFMUSC op = OpcodeFMUSC();
		if (!requireU8("musicID", op.musicID)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CMUSC: {
		OpcodeCMUSC op = OpcodeCMUSC();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("musicID", op.musicID)) return false;
		if (!requireU8("opcode", op.opcode)) return false;
		if (!requireU16("param1", op.param1)) return false;
		if (!requireU16("param2", op.param2)) return false;
		out = Opcode(op);
		return true;
	}
	case OpcodeKey::CHMST: {
		OpcodeCHMST op = OpcodeCHMST();
		if (!readBanks(params, 1, &op.banks, errorStr, lineNo)) return false;
		if (!requireU8("var", op.var)) return false;
		out = Opcode(op);
		return true;
	}
	default:
		break;
	}

	if (errorStr) {
		*errorStr = QString("Line %1: unsupported or missing parameters for opcode '%2'.")
		                .arg(lineNo)
		                .arg(QString::fromLatin1(Opcode::names[int(key)]));
	}
	return false;
}
}

void Section1File::clear()
{
	_grpScripts.clear();
	_texts.clear();
	_author.clear();

	setOpen(false);
}

void Section1File::initEmpty()
{
	clear();

	_author = "makou";
	_scale = 512;
	_version = 0x0502;

	_texts.append(FF7String(QObject::tr("Map name"), false));
	_texts.append(FF7String(QObject::tr("Hello world!"), false));

	QList<Script> scripts;
	QList<Opcode> opcodes;

	opcodes.append(OpcodeMPNAM());
	opcodes.append(OpcodeRET());
	scripts.append(Script(opcodes));

	QSize s = FF7Font::calcSize(_texts.at(1).data());

	opcodes.clear();
	OpcodeWINDOW opcodeWINDOW;
	opcodeWINDOW.targetX = 0;
	opcodeWINDOW.targetY = 0;
	opcodeWINDOW.width = quint16(s.width());
	opcodeWINDOW.height = quint16(s.height());
	opcodeWINDOW.windowID = 0;
	opcodes.append(opcodeWINDOW);
	OpcodeMESSAGE opcodeMESSAGE;
	opcodeMESSAGE.textID = 1;
	opcodeMESSAGE.windowID = 0;
	opcodes.append(opcodeMESSAGE);
	opcodes.append(OpcodeRET());
	scripts.append(Script(opcodes));

	opcodes.clear();
	opcodes.append(OpcodeRET());
	scripts.append(Script(opcodes));

	_grpScripts.append(GrpScript("dic", scripts));

	QMap<quint8, QString> chars;
	chars.insert(0, "cloud");
	chars.insert(2, "tifa");
	chars.insert(8, "cid");

	QMapIterator<quint8, QString> it(chars);
	quint8 modelID = 0;

	while (it.hasNext()) {
		it.next();

		scripts.clear();

		opcodes.clear();
		OpcodeCHAR_ opcodeCHAR;
		opcodeCHAR.object3DID = modelID;
		opcodes.append(opcodeCHAR);
		OpcodePC opcodePC;
		opcodePC.charID = it.key();
		opcodes.append(opcodePC);
		opcodes.append(OpcodeRET());
		scripts.append(Script(opcodes));

		opcodes.clear();
		opcodes.append(OpcodeRET());
		scripts.append(Script(opcodes));

		opcodes.clear();
		opcodes.append(OpcodeRET());
		scripts.append(Script(opcodes));

		_grpScripts.append(GrpScript(it.value(), scripts));

		modelID += 1;
	}
}

bool Section1File::open()
{
	return open(field()->sectionData(Field::Scripts));
}

bool Section1File::open(const QByteArray &data)
{
	quint16 version, posTexts;
	int cur;
	qsizetype dataSize = data.size();
	const char *constData = data.constData();
	bool isDemo;

	if (dataSize < 32) {
		qWarning() << "Section1File::open data too short" << dataSize;
		return false;
	}

	memcpy(&version, constData, 2);

	_version = version;

	isDemo = version == 0x0301; // Check version format

	memcpy(&posTexts, constData + 4, 2); // posTexts (and end of scripts)
	if (quint32(dataSize) < posTexts || posTexts < 32) {
		qWarning() << "Section1File::open out of range posTexts" << posTexts << dataSize;
		return false;
	}

	clear();

	/* ---------- SCRIPTS ---------- */

	quint32 posAKAO = 0;
	quint16 nbAKAO, pos;
	quint8 emptyGrps = 0, nbScripts = quint8(data.at(2));

	// quint8 count3DModel = quint8(data.at(3));
	memcpy(&nbAKAO, constData + 6, 2); // nbAKAO

	if (isDemo) {
		_scale = 0; // FIXME: better value?
		cur = 8;
	} else {
		memcpy(&_scale, constData + 8, 2);
		cur = 16;
	}
	const char *author = constData + cur;
	_author = QString::fromLatin1(author, qsizetype(qstrnlen(author, 8)));
	//QString name2 = data.mid(cur + 8, 8);
	cur += 16;

	int posScripts = cur + 8 * nbScripts + 4 * nbAKAO;

	if (posTexts < posScripts + 64 * nbScripts) {
		qWarning() << "Section1File::open out of range posScripts" << posTexts << posScripts << nbScripts << dataSize;
		return false;
	}

	quint16 positions[33];
	const quint8 scriptCount = isDemo ? 16 : 32;

	if (nbAKAO > 0) {
		//INTERGRITY TEST
		//		QString out;
		//		bool pasok = false;
		//		for (int i=0; i<nbAKAO; ++i) {
		//			memcpy(&posAKAO, &constData[cur+8*nbScripts+i*4], 4);
		//			out.append(QString("%1 %2 %3 (%4)\n").arg(i).arg(posAKAO).arg(QString(data.mid(posAKAO, 4))).arg(QString(data.mid(posAKAO-4, 8).toHex())));
		//			if (data.mid(posAKAO, 4) != "AKAO" && data.at(posAKAO) != '\x12') {
		//				pasok = true;
		//			}
		//		}
		//		if (pasok) {
		//			qDebug() << out;
		//		}

		memcpy(&posAKAO, constData + cur + 8 * nbScripts, 4); // posAKAO
	} else {
		posAKAO = quint32(dataSize);
	}

	// On the Android version, posTexts can be after posAKAO
	quint16 posAfterScripts = quint16(qMin(posAKAO, quint32(posTexts)));
	qsizetype posAfterScriptsAndroid = -1;
	
	// Try to detect "posAfterScripts" with more accuracy
	if (posAKAO < quint32(posTexts) && posTexts + 4 < dataSize) {
		posAfterScriptsAndroid = data.lastIndexOf(QByteArrayView(constData + posTexts, 4), posAKAO);
	}

	for (quint8 i = 0; i < nbScripts; ++i) {
		const char *grpName = constData + cur + 8 * i;
		GrpScript grpScript(QString::fromLatin1(grpName, qsizetype(qstrnlen(grpName, 8))));

		if (emptyGrps > 1) {
			emptyGrps--;
		} else {
			// Listing start offsets
			memcpy(positions, constData + posScripts + scriptCount * 2 * i, scriptCount * 2);

			// Add offset at the end
			if (i == nbScripts - 1) {
				positions[scriptCount] = posAfterScriptsAndroid >= positions[scriptCount - 1] ? posAfterScriptsAndroid : posAfterScripts;
			} else {
				memcpy(&pos, constData + posScripts + scriptCount * 2 * (i + 1), 2);

				if (pos > positions[scriptCount - 1]) {
					positions[scriptCount] = pos;
				} else {
					emptyGrps = 1;
					while (pos <= positions[scriptCount - 1] && i+emptyGrps<nbScripts-1) {
						memcpy(&pos, constData + posScripts + scriptCount * 2 * (i + emptyGrps + 1), 2);
						emptyGrps++;
					}
					if (i + emptyGrps == nbScripts) {
						positions[scriptCount] = posAfterScriptsAndroid >= positions[scriptCount - 1] ? posAfterScriptsAndroid : posAfterScripts;
					} else {
						positions[scriptCount] = pos;
					}
				}
			}

			quint8 scriptID = 0;
			for (quint8 j = 0; j < scriptCount; ++j) {
				if (positions[j + 1] > positions[j]) {
					Script script(constData + positions[j], positions[j + 1] - positions[j]);
					if (!script.isValid()) {
						qWarning() << "Section1File::open invalid script" << i << j;
						return false;
					}
					if (scriptID == 0) {
						Script scriptMain = script.splitScriptAtReturn();
						grpScript.setScript(0, script); // S0 - Init
						grpScript.setScript(1, scriptMain); // S0 - Main
					} else {
						grpScript.setScript(scriptID + 1, script);
					}
					scriptID = j + 1;
				}
			}
		}
		
		_grpScripts.append(grpScript);
	}

	qsizetype sizeTextSection;

	/* ---------- TEXTS ---------- */
	if (posAKAO >= posTexts) {
		sizeTextSection = posAKAO - posTexts;
	} else {
		sizeTextSection = dataSize - posTexts;
	}

	if (sizeTextSection > 4) { // If there are texts
		quint16 posBeg, posEnd, textCount;
		if (dataSize < posTexts + 2) {
			qWarning() << "Section1File::open invalid posTexts 2" << posTexts << dataSize;
			return false;
		}
		memcpy(&posBeg, constData + posTexts + 2, 2);
		if (posBeg > 0) {
			textCount = posBeg / 2 - 1;

			for (quint32 i = 1; i < textCount; ++i) {
				memcpy(&posEnd, constData + posTexts + 2 + i*2, 2);

				if (dataSize < posTexts + posEnd) {
					qWarning() << "Section1File::open invalid posFin" << posTexts << posEnd << dataSize;
					break;
				}

				// FIXME: possible hidden data between 0xFF and posEnd - posBeg
				_texts.append(FF7String(QByteArrayView(constData + posTexts + posBeg, posEnd - posBeg)));
				posBeg = posEnd;
			}
			if (dataSize < sizeTextSection) {
				qWarning() << "Section1File::open invalid sizeTextSection" << sizeTextSection << dataSize;
				return false;
			}
			// FIXME: possible hidden data between 0xFF and posEnd - posBeg
			_texts.append(FF7String(QByteArrayView(constData + posTexts + posBeg, sizeTextSection - posBeg)));
		}
	}

	setOpen(true);

	return true;
}

QByteArray Section1File::save() const
{
	TutFileStandard *tut = field()->tutosAndSounds();
	QByteArray grpScriptNames, positionsScripts, positionsAKAO, allScripts, realScript, positionsTexts, allTexts, allAKAOs;
	quint32 newPosAKAOs, pos32, newPosTexts32;
	quint16 newPosScripts, newPosTexts, newNbAKAO, pos;
	quint8 newNbGrpScripts;

	//nbGrpScripts = (quint8)data.at(2);//nbGrpScripts
	newNbGrpScripts = _grpScripts.size();
	//memcpy(&posTexts, constData + 4, 2);//posTexts (and end of the scripts section)

	newNbAKAO = tut->size(); // 255 maximum

	newPosScripts = 32 + newNbGrpScripts * 72 + newNbAKAO * 4;
	pos32 = newPosScripts;

	// Creation newPosScripts + scripts
	quint8 count3DModel = 0;
	for (const GrpScript &grpScript : _grpScripts) {
		grpScriptNames.append(grpScript.realName().toLatin1().leftJustified(8, '\x00', true));
		for (quint8 j = 0; j < 32; ++j) {
			realScript = grpScript.toByteArray(j);
			if (!realScript.isEmpty()) {
				pos32 = newPosScripts + allScripts.size();
			}
			if (pos32 > 65535) {
				qWarning() << "Section1File::save script size overflow";
				return QByteArray();
			}
			pos = quint16(pos32);
			positionsScripts.append((char *)&pos, 2);
			allScripts.append(realScript);
		}
		if (grpScript.type() == GrpScript::Model) {
			++count3DModel;
		}
	}

	// Creation new positions Texts
	newPosTexts32 = newPosScripts + allScripts.size();
	if (newPosTexts32 > 65535) {
		qWarning() << "Section1File::save script size overflow";
		return QByteArray();
	}
	newPosTexts = quint16(newPosTexts32);

	quint16 newNbText = textCount();

	for (const FF7String &text : texts()) {
		pos32 = 2 + newNbText * 2 + allTexts.size();
		if (pos32 > 65535) {
			qWarning() << "Section1File::save script + text size overflow";
			return QByteArray();
		}
		pos = quint16(pos32);
		positionsTexts.append((char *)&pos, 2);
		allTexts.append(text.data());
		allTexts.append('\xff');// end of text
	}

	// Word padding
	int scriptsAndTextsSize = positionsScripts.size() + allScripts.size() + 2 + positionsTexts.size() + allTexts.size();
	if (scriptsAndTextsSize % 4 != 0 && tut->size() > 0) {
		allTexts.append(QByteArray(4 - scriptsAndTextsSize % 4, '\0'));
	}

	newPosAKAOs = newPosTexts + (2 + newNbText*2 + allTexts.size());

	allAKAOs = tut->save(positionsAKAO, newPosAKAOs);

	QByteArray mapauthor = _author.toLatin1().leftJustified(8, '\0', true);
	mapauthor[7] = '\0';
	QByteArray mapname = field()->name().toLatin1().leftJustified(8, '\0', true);
	mapname[7] = '\0';

	return QByteArray()
	    .append((char *)&_version, 2) // Version
	    .append(char(newNbGrpScripts)) // nbGrpScripts
	    .append(char(count3DModel)) // nb3DObjects
	    .append((char *)&newPosTexts, 2) // PosTexts
	    .append((char *)&newNbAKAO, 2) // AKAO count
	    .append((char *)&_scale, 2)
	    .append(_empty.leftJustified(6, '\0', true)) // Empty
	    .append(mapauthor) // mapAuthor
	    .append(mapname) // mapName
	    .append(grpScriptNames) // Names of grpScripts
	    .append(positionsAKAO) // PosAKAO
	    .append(positionsScripts) // PosScripts
	    .append(allScripts) // Scripts
	    .append((char *)&newNbText, 2) // nbTexts
	    .append(positionsTexts) // positionsTexts
	    .append(allTexts) // Texts
	    .append(allAKAOs); // AKAO / tutos
}

bool Section1File::exporter(QIODevice *device, ExportFormat format)
{
	switch (format) {
	case TXTText: {
		if (!device->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
			return false;
		}
		int i=0;
		for (const FF7String &text : texts()) {
			device->write(QString("---TEXT%1---\n%2\n")
						  .arg(i++, 3, 10, QChar('0'))
						  .arg(text.text())
						  .toUtf8());
		}
		device->close();
		return true;
	}
	case XMLText: {
		if (!device->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
			return false;
		}
		
		QStringList charNames;
		QString mapName = field()->name();
		
		if (field()->isPC()) {
			FieldModelLoaderPC *modelLoader = static_cast<FieldModelLoaderPC *>(field()->fieldModelLoader());
			if (modelLoader != nullptr) {
				charNames = modelLoader->charNames();
			}
			InfFile *inf = field()->inf();
			if (inf != nullptr) {
				mapName = inf->mapName();
			}
		}
		
		QXmlStreamWriter stream(device);
		stream.setAutoFormatting(true);
		stream.writeStartDocument();
		stream.writeStartElement("field");
		stream.writeAttribute("name", field()->name());
		stream.writeStartElement("texts");
		int id = 0;
		for (const FF7String &text : texts()) {
			stream.writeStartElement("text");
			stream.writeAttribute("id", QString::number(id));
			QList<FF7Window> windows;
			listWindows(id, windows);
			if (!windows.empty()) {
				QString groupName, modelName;
				for (const FF7Window &win: windows) {
					const int groupId = win.groupID;
					const GrpScript &grpScript = _grpScripts.at(groupId);
					groupName = grpScript.name();
					const int modelId = modelID(groupId);
					
					if (modelId >= 0 && modelId < charNames.size()) {
						modelName = charNames.at(modelId);
						if (modelName.startsWith(mapName)) {
							modelName = modelName.mid(mapName.size());
						}
						if (modelName.endsWith(".char")) {
							modelName = modelName.left(modelName.size() - 5);
						}
						break;
					}
				}
				stream.writeAttribute("group", groupName);
				if (!modelName.isEmpty()) {
					stream.writeAttribute("model", modelName);
				}
				
			}
			stream.writeCharacters(text.text());
			stream.writeEndElement(); // /text
			++id;
		}

		stream.writeEndElement(); // /texts
		stream.writeEndElement(); // /field
		stream.writeEndDocument();
		device->close();
		return true;
	}
	}

	return false;
}

bool Section1File::exportScripts(QIODevice *device) const
{
	if (!device->open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
		return false;
	}

	QTextStream stream(device);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	stream.setEncoding(QStringConverter::Utf8);
#else
	stream.setCodec("UTF-8");
#endif

	stream << "field name=\"" << escapeScriptString(field()->name()) << "\"";
	if (!_author.isEmpty()) {
		stream << " author=\"" << escapeScriptString(_author) << "\"";
	}
	stream << " version=" << _version << "\n";

	int groupID = 0;
	for (const GrpScript &group : _grpScripts) {
		stream << "group id=" << groupID << " name=\"" << escapeScriptString(group.name()) << "\"";
		QString typeString = group.typeString();
		if (!typeString.isEmpty()) {
			stream << " ; " << typeString;
		}
		stream << "\n";

		for (int scriptID = 0; scriptID < group.scripts().size(); ++scriptID) {
			const Script &script = group.script(quint8(scriptID));
			stream << "\t" << "script id=" << scriptID
			       << " name=\"" << escapeScriptString(group.scriptName(quint8(scriptID))) << "\"\n";
			for (const Opcode &opcode : script.opcodes()) {
				stream << "\t\t";
				if (opcode.id() == OpcodeKey::LABEL) {
					stream << "LABEL id=" << opcode.op().opcodeLABEL._label << "\n";
				} else {
					stream << opcode.name();
					QString namedParams = opcodeNamedParams(opcode);
					if (!namedParams.isEmpty()) {
						stream << " " << namedParams;
					}
					stream << "\n";
				}
			}
		}
		++groupID;
	}

	for (int textID = 0; textID < _texts.size(); ++textID) {
		stream << "text id=" << textID << " content=\""
		       << escapeScriptString(_texts.at(textID).text()) << "\"\n";
	}

	device->close();
	return true;
}

bool Section1File::importScripts(QIODevice *device, QString *errorStr)
{
	if (!device->open(QIODevice::ReadOnly | QIODevice::Text)) {
		if (errorStr) {
			*errorStr = device->errorString();
		}
		return false;
	}

	QTextStream stream(device);
#if QT_VERSION >= QT_VERSION_CHECK(6, 0, 0)
	stream.setEncoding(QStringConverter::Utf8);
#else
	stream.setCodec("UTF-8");
#endif

	struct ParsedGroup {
		QString name;
		QMap<int, QList<Opcode>> scripts;
	};

	QHash<int, ParsedGroup> parsedGroups;
	QMap<int, QString> parsedTexts;
	QString parsedFieldName;
	QString parsedAuthor;
	bool authorSpecified = false;
	qint64 parsedVersion = -1;

	int currentGroupId = -1;
	int currentScriptId = -1;
	bool seenField = false;

	const auto setError = [&](const QString &message) {
		if (errorStr) {
			*errorStr = message;
		}
	};

	int lineNo = 0;
	while (!stream.atEnd()) {
		QString line = stream.readLine();
		++lineNo;
		if (lineNo == 1 && !line.isEmpty() && line.at(0).unicode() == 0xFEFF) {
			line = line.mid(1);
		}
		QString trimmed = line.trimmed();
		if (trimmed.isEmpty()) {
			continue;
		}

		QString token = trimmed.section(' ', 0, 0);
		QString rest = trimmed.mid(token.size()).trimmed();

		if (token == "field") {
			if (seenField) {
				setError(QString("Line %1: duplicate field header.").arg(lineNo));
				return false;
			}
			QMap<QString, QString> params;
			if (!parseKeyValueList(rest, params, errorStr, lineNo)) {
				return false;
			}
			if (!params.contains("name") || !params.contains("version")) {
				setError(QString("Line %1: field header requires name and version.").arg(lineNo));
				return false;
			}
			parsedFieldName = params.value("name");
			qint64 version = 0;
			if (!parseNumber(params.value("version"), version) || version < 0 || version > 0xFFFF) {
				setError(QString("Line %1: invalid field version.").arg(lineNo));
				return false;
			}
			parsedVersion = version;
			if (params.contains("author")) {
				parsedAuthor = params.value("author");
				authorSpecified = true;
			}
			seenField = true;
			continue;
		}

		if (!seenField) {
			setError(QString("Line %1: missing field header.").arg(lineNo));
			return false;
		}

		if (token == "group") {
			QMap<QString, QString> params;
			const int commentIndex = rest.indexOf(';');
			const QString withoutComment = commentIndex >= 0 ? rest.left(commentIndex).trimmed() : rest;
			if (!parseKeyValueList(withoutComment, params, errorStr, lineNo)) {
				return false;
			}
			int groupId = -1;
			if (!params.contains("id") || !params.contains("name")) {
				setError(QString("Line %1: group requires id and name.").arg(lineNo));
				return false;
			}
			qint64 groupIdValue = 0;
			if (!parseNumber(params.value("id"), groupIdValue) || groupIdValue < 0 || groupIdValue > 0xFF) {
				setError(QString("Line %1: invalid group id.").arg(lineNo));
				return false;
			}
			groupId = int(groupIdValue);
			if (parsedGroups.contains(groupId)) {
				setError(QString("Line %1: duplicate group id %2.").arg(lineNo).arg(groupId));
				return false;
			}
			ParsedGroup group;
			group.name = params.value("name");
			parsedGroups.insert(groupId, group);
			currentGroupId = groupId;
			currentScriptId = -1;
			continue;
		}

		if (token == "script") {
			if (currentGroupId < 0) {
				setError(QString("Line %1: script declared before group.").arg(lineNo));
				return false;
			}
			QMap<QString, QString> params;
			if (!parseKeyValueList(rest, params, errorStr, lineNo)) {
				return false;
			}
			if (!params.contains("id")) {
				setError(QString("Line %1: script requires id.").arg(lineNo));
				return false;
			}
			qint64 scriptIdValue = 0;
			if (!parseNumber(params.value("id"), scriptIdValue) || scriptIdValue < 0 || scriptIdValue >= SCRIPTS_SIZE) {
				setError(QString("Line %1: invalid script id.").arg(lineNo));
				return false;
			}
			const int scriptId = int(scriptIdValue);
			ParsedGroup &group = parsedGroups[currentGroupId];
			if (group.scripts.contains(scriptId)) {
				setError(QString("Line %1: duplicate script id %2 in group %3.")
				         .arg(lineNo)
				         .arg(scriptId)
				         .arg(currentGroupId));
				return false;
			}
			group.scripts.insert(scriptId, QList<Opcode>());
			currentScriptId = scriptId;
			continue;
		}

		if (token == "text") {
			QMap<QString, QString> params;
			if (!parseKeyValueList(rest, params, errorStr, lineNo)) {
				return false;
			}
			if (!params.contains("id") || !params.contains("content")) {
				setError(QString("Line %1: text requires id and content.").arg(lineNo));
				return false;
			}
			qint64 textIdValue = 0;
			if (!parseNumber(params.value("id"), textIdValue) || textIdValue < 0 || textIdValue > 0xFFFF) {
				setError(QString("Line %1: invalid text id.").arg(lineNo));
				return false;
			}
			const int textId = int(textIdValue);
			if (parsedTexts.contains(textId)) {
				setError(QString("Line %1: duplicate text id %2.").arg(lineNo).arg(textId));
				return false;
			}
			parsedTexts.insert(textId, params.value("content"));
			continue;
		}

		if (currentGroupId < 0 || currentScriptId < 0) {
			setError(QString("Line %1: opcode declared before script.").arg(lineNo));
			return false;
		}

		QString opcodeName = token;
		QMap<QString, QString> params;
		if (!rest.isEmpty() && !parseKeyValueList(rest, params, errorStr, lineNo)) {
			return false;
		}

		Opcode opcode;
		if (opcodeName.compare("LABEL", Qt::CaseInsensitive) == 0) {
			quint16 label = 0;
			if (!readLabelTarget(params, label, errorStr, lineNo)) {
				return false;
			}
			OpcodeLABEL op = OpcodeLABEL();
			op._label = label;
			opcode = Opcode(op);
		} else {
			static QHash<QString, OpcodeKey> opcodeLookup;
			if (opcodeLookup.isEmpty()) {
				for (int i = 0; i < 257; ++i) {
					opcodeLookup.insert(QString::fromLatin1(Opcode::names[i]).toUpper(), OpcodeKey(i));
				}
			}
			const QString lookupKey = opcodeName.toUpper();
			if (!opcodeLookup.contains(lookupKey)) {
				setError(QString("Line %1: unknown opcode '%2'.").arg(lineNo).arg(opcodeName));
				return false;
			}
			const OpcodeKey key = opcodeLookup.value(lookupKey);
			if (!buildOpcodeFromParams(key, params, opcode, errorStr, lineNo)) {
				return false;
			}
		}

		ParsedGroup &group = parsedGroups[currentGroupId];
		group.scripts[currentScriptId].append(opcode);
	}

	device->close();

	if (!seenField) {
		setError("Missing field header.");
		return false;
	}
	if (parsedFieldName.isEmpty() || parsedVersion < 0) {
		setError("Missing field name or version.");
		return false;
	}
	if (parsedFieldName != field()->name()) {
		setError(QString("Field name mismatch: expected '%1', got '%2'.")
		         .arg(field()->name(), parsedFieldName));
		return false;
	}

	for (auto it = parsedGroups.constBegin(); it != parsedGroups.constEnd(); ++it) {
		const int groupId = it.key();
		if (groupId < 0 || groupId >= _grpScripts.size()) {
			setError(QString("Group id out of range: %1").arg(groupId));
			return false;
		}

		QMap<int, Script> compiledScripts;
		for (auto scriptIt = it->scripts.constBegin(); scriptIt != it->scripts.constEnd(); ++scriptIt) {
			Script script(scriptIt.value());
			int opcodeId = 0;
			QString compileError;
			if (!script.compile(opcodeId, compileError)) {
				setError(QString("Group %1 script %2: %3").arg(groupId).arg(scriptIt.key()).arg(compileError));
				return false;
			}
			compiledScripts.insert(scriptIt.key(), script);
		}

		GrpScript &group = _grpScripts[groupId];
		group.setName(it->name);
		for (int scriptId = 0; scriptId < SCRIPTS_SIZE; ++scriptId) {
			if (compiledScripts.contains(scriptId)) {
				group.setScript(scriptId, compiledScripts.value(scriptId));
			} else {
				group.setScript(scriptId, Script());
			}
		}
	}

	if (!parsedTexts.isEmpty()) {
		bool jp = Config::value("jp_txt", false).toBool();
		const int maxId = parsedTexts.lastKey();
		_texts.clear();
		_texts.reserve(maxId + 1);
		for (int i = 0; i <= maxId; ++i) {
			if (parsedTexts.contains(i)) {
				_texts.append(FF7String(parsedTexts.value(i), jp));
			} else {
				_texts.append(FF7String());
			}
		}
	}

	if (authorSpecified) {
		_author = parsedAuthor;
	}
	_version = quint16(parsedVersion);
	setModified(true);
	return true;
}

bool Section1File::importer(QIODevice *device, ExportFormat format)
{
	Q_UNUSED(format)
	//TODO
	// bool jp = Config::value("jp_txt", false).toBool();
	bool start = false, field = false, texts = false;

	QXmlStreamReader stream(device);

	while (!stream.atEnd()) {
		QXmlStreamReader::TokenType type = stream.readNext();
		if (!start && type == QXmlStreamReader::StartDocument) {
			start = true;
		} else if (start && !field && type == QXmlStreamReader::StartElement
				  && stream.name() == QLatin1String("field")) {
			field = true;
		} else if (field && !texts && type == QXmlStreamReader::StartElement
				  && stream.name() == QLatin1String("texts")) {
			texts = true;
		} else if (texts && type == QXmlStreamReader::StartElement
				  && stream.name() == QLatin1String("text")) {
//			stream.attributes().value("id");
		}
	}

	return stream.hasError();
}

bool Section1File::isModified() const
{
	TutFileStandard *tut = field()->tutosAndSounds();
	return FieldPart::isModified() || (tut && tut->isModified());
}

int Section1File::modelID(quint8 grpScriptID) const
{
	if (_grpScripts.at(grpScriptID).type() != GrpScript::Model) {
		return -1;
	}

	int ID = 0;

	for (int i = 0; i < grpScriptID; ++i) {
		if (_grpScripts.at(i).type() == GrpScript::Model) {
			++ID;
		}
	}

	return ID;
}

void Section1File::bgParamAndBgMove(QHash<quint8, quint8> &paramActifs, qint16 *z, qint16 *x, qint16 *y) const
{
	for (const GrpScript &grpScript : _grpScripts) {
		grpScript.backgroundParams(paramActifs);
		if (z) {
			grpScript.backgroundMove(z, x, y);
		}
	}
}

const QList<GrpScript> &Section1File::grpScripts() const
{
	return _grpScripts;
}

const GrpScript &Section1File::grpScript(int groupID) const
{
	return _grpScripts.at(groupID);
}

GrpScript &Section1File::grpScript(int groupID)
{
	return _grpScripts[groupID];
}

qsizetype Section1File::grpScriptCount() const
{
	return _grpScripts.size();
}

bool Section1File::insertGrpScript(int row, const GrpScript &grpScript)
{
	if (grpScriptCount() < maxGrpScriptCount()) {
		_grpScripts.insert(row, grpScript);
		for (GrpScript &grpScript : _grpScripts) {
			grpScript.shiftGroupIds(row - 1, +1);
		}
		setModified(true);
		return true;
	}
	return false;
}

void Section1File::removeGrpScript(int row)
{
	if (row < _grpScripts.size()) {
		_grpScripts.removeAt(row);
		for (GrpScript &grpScript : _grpScripts) {
			grpScript.shiftGroupIds(row, -1);
		}
		setModified(true);
	}
}

bool Section1File::moveGrpScript(int row, bool direction)
{
	if (row >= _grpScripts.size()) {
		return false;
	}

	if (direction) { // down
		if (row == _grpScripts.size() - 1) {
			return false;
		}
		_grpScripts.swapItemsAt(row, row + 1);
		for (GrpScript &grpScript : _grpScripts) {
			grpScript.swapGroupIds(row, row + 1);
		}
	} else { // up
		if (row == 0) {
			return false;
		}
		_grpScripts.swapItemsAt(row, row - 1);
		for (GrpScript &grpScript : _grpScripts) {
			grpScript.swapGroupIds(row, row - 1);
		}
	}
	setModified(true);

	return true;
}

void Section1File::searchAllVars(QList<FF7Var> &vars) const
{
	for (const GrpScript &group : _grpScripts) {
		group.searchAllVars(vars);
	}
}

bool Section1File::searchOpcode(int opcode, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID < 0) {
		groupID = scriptID = opcodeID = 0;
	}
	if (groupID >= _grpScripts.size()) {
		return false;
	}
	if (_grpScripts.at(groupID).searchOpcode(opcode, scriptID, opcodeID)) {
		return true;
	}

	scriptID = 0;
	opcodeID = 0;
	return searchOpcode(opcode, ++groupID, scriptID, opcodeID);
}

bool Section1File::searchVar(quint8 bank, quint16 address, Opcode::Operation op, int value, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID < 0) {
		groupID = scriptID = opcodeID = 0;
	}
	if (groupID >= _grpScripts.size()) {
		return false;
	}
	if (_grpScripts.at(groupID).searchVar(bank, address, op, value, scriptID, opcodeID)) {
		return true;
	}

	scriptID = 0;
	opcodeID = 0;
	return searchVar(bank, address, op, value, ++groupID, scriptID , opcodeID);
}

bool Section1File::searchExec(quint8 group, quint8 script, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID < 0) {
		groupID = scriptID = opcodeID = 0;
	}
	if (groupID >= _grpScripts.size()) {
		return false;
	}
	if (_grpScripts.at(groupID).searchExec(group, script, scriptID, opcodeID)) {
		return true;
	}

	scriptID = 0;
	opcodeID = 0;
	return searchExec(group, script, ++groupID, scriptID, opcodeID);
}

bool Section1File::searchMapJump(quint16 map, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID < 0) {
		groupID = scriptID = opcodeID = 0;
	}
	if (groupID >= _grpScripts.size()) {
		return false;
	}
	if (_grpScripts.at(groupID).searchMapJump(map, scriptID, opcodeID)) {
		return true;
	}

	scriptID = 0;
	opcodeID = 0;
	return searchMapJump(map, ++groupID, scriptID, opcodeID);
}

bool Section1File::searchTextInScripts(const QRegularExpression &text, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID < 0) {
		groupID = scriptID = opcodeID = 0;
	}
	if (groupID >= _grpScripts.size()) {
		return false;
	}
	if (_grpScripts.at(groupID).searchTextInScripts(text, scriptID, opcodeID, this)) {
		return true;
	}

	scriptID = 0;
	opcodeID = 0;
	return searchTextInScripts(text, ++groupID, scriptID, opcodeID);
}

bool Section1File::searchText(const QRegularExpression &text, int &textID, qsizetype &from, qsizetype &size) const
{
	if (textID < 0) {
		textID = 0;
	}
	if (textID >= textCount()) {
		return false;
	}
	from = this->text(textID).indexOf(text, from, size);
	if (from != -1) {
		return true;
	}

	from = 0;
	return searchText(text, ++textID, from, size);
}

bool Section1File::searchOpcodeP(int opcode, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID >= _grpScripts.size()) {
		groupID = _grpScripts.size() - 1;
		scriptID = opcodeID = 2147483647;
	}
	if (groupID < 0) {
		return false;
	}
	if (_grpScripts.at(groupID).searchOpcodeP(opcode, scriptID, opcodeID)) {
		return true;
	}

	groupID -= 1;
	scriptID = 2147483647;
	opcodeID = 2147483647;
	return searchOpcodeP(opcode, groupID, scriptID, opcodeID);
}

bool Section1File::searchVarP(quint8 bank, quint16 address, Opcode::Operation op, int value, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID >= _grpScripts.size()) {
		groupID = _grpScripts.size() - 1;
		scriptID = opcodeID = 2147483647;
	}
	if (groupID < 0) {
		return false;
	}
	if (_grpScripts.at(groupID).searchVarP(bank, address, op, value, scriptID, opcodeID)) {
		return true;
	}

	groupID -= 1;
	scriptID = 2147483647;
	opcodeID = 2147483647;
	return searchVarP(bank, address, op, value, groupID, scriptID, opcodeID);
}

bool Section1File::searchExecP(quint8 group, quint8 script, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID >= _grpScripts.size()) {
		groupID = _grpScripts.size() - 1;
		scriptID = opcodeID = 2147483647;
	}
	if (groupID < 0) {
		return false;
	}
	if (_grpScripts.at(groupID).searchExecP(group, script, scriptID, opcodeID)) {
		return true;
	}

	groupID -= 1;
	scriptID = 2147483647;
	opcodeID = 2147483647;
	return searchExecP(group, script, groupID, scriptID, opcodeID);
}

bool Section1File::searchMapJumpP(quint16 map, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID >= _grpScripts.size()) {
		groupID = _grpScripts.size() - 1;
		scriptID = opcodeID = 2147483647;
	}
	if (groupID < 0) {
		return false;
	}
	if (_grpScripts.at(groupID).searchMapJumpP(map, scriptID, opcodeID)) {
		return true;
	}

	groupID -= 1;
	scriptID = 2147483647;
	opcodeID = 2147483647;
	return searchMapJumpP(map, groupID, scriptID, opcodeID);
}

bool Section1File::searchTextInScriptsP(const QRegularExpression &text, int &groupID, int &scriptID, int &opcodeID) const
{
	if (groupID >= _grpScripts.size()) {
		groupID = _grpScripts.size() - 1;
		scriptID = opcodeID = 2147483647;
	}
	if (groupID < 0) {
		return false;
	}
	if (_grpScripts.at(groupID).searchTextInScriptsP(text, scriptID, opcodeID, this)) {
		return true;
	}

	groupID -= 1;
	scriptID = 2147483647;
	opcodeID = 2147483647;
	return searchTextInScriptsP(text, groupID, scriptID, opcodeID);
}

bool Section1File::searchTextP(const QRegularExpression &text, int &textID, qsizetype &from, qsizetype &index, qsizetype &size) const
{
	if (textID >= textCount()) {
		textID = textCount()-1;
		from = -1;
	}
	if (textID < 0) {
		return false;
	}
	index = this->text(textID).lastIndexOf(text, from, size);
	if (index != -1) {
		return true;
	}

	textID -= 1;
	from = -1;
	return searchTextP(text, textID, from, index, size);
}

bool Section1File::replaceText(const QRegularExpression &search, const QString &after, int textID, int from)
{
	bool jp = Config::value("jp_txt", false).toBool();
	FF7String beforeT = text(textID);
	QString before = beforeT.text();
	QRegularExpressionMatch match = search.match(before, from);

	if (match.capturedStart() == from) {
		before.replace(from, match.capturedLength(), after);
		setText(textID, FF7String(before, jp));
		return true;
	}
	return false;
}

void Section1File::setWindow(const FF7Window &win)
{
	if (win.groupID < _grpScripts.size()) {
		_grpScripts[win.groupID].setWindow(win);
		setModified(true);
	}
}

void Section1File::listWindows(QMultiMap<quint64, FF7Window> &windows, QMultiMap<quint8, quint64> &text2win) const
{
	int groupID = 0;
	for (const GrpScript &group : _grpScripts) {
		group.listWindows(groupID++, windows, text2win);
	}
}

void Section1File::listWindows(int textID, QList<FF7Window> &windows, int winID) const
{
	int groupID = 0;
	for (const GrpScript &group : _grpScripts) {
		group.listWindows(groupID++, textID, windows, winID);
	}
}

void Section1File::listModelPositions(QMultiMap<int, FF7Position> &positions) const
{
	int modelId = 0;
	for (const GrpScript &group : _grpScripts) {
		if (group.type() == GrpScript::Model) {
			QList<FF7Position> pos;
			group.listModelPositions(pos);
			if (!pos.isEmpty()) {
				for (const FF7Position &position : std::as_const(pos)) {
					positions.insert(modelId, position);
				}
			}
			++modelId;
		}
	}
}

int Section1File::modelCount() const
{
	int modelId = 0;
	for (const GrpScript &group : grpScripts()) {
		if (group.type() == GrpScript::Model) {
			modelId++;
		}
	}

	return modelId;
}

void Section1File::linePosition(QMap<int, std::pair<FF7Position, FF7Position>> &positions) const
{
	int groupID = 0;
	for (const GrpScript &group : _grpScripts) {
		if (group.type() == GrpScript::Location) {
			FF7Position position[2] = { FF7Position(), FF7Position() };
			if (group.linePosition(position)) {
				positions.insert(groupID, std::pair<FF7Position, FF7Position>(position[0], position[1]));
			}
		}
		++groupID;
	}
}

bool Section1File::compileScripts(int &groupID, int &scriptID, int &opcodeID, QString &errorStr)
{
	groupID = 0;
	for (GrpScript &group : _grpScripts) {
		if (!group.compile(scriptID, opcodeID, errorStr)) {
			return false;
		}
		++groupID;
	}

	return true;
}

void Section1File::removeTexts()
{
	for (GrpScript &group : _grpScripts) {
		if (group.removeTexts()) {
			setModified(true);
		}
	}
}

void Section1File::cleanTexts()
{
	QSet<quint8> usedTexts = listUsedTexts();

	for (int textID = 0; textID < _texts.size(); ++textID) {
		if (!usedTexts.contains(quint8(textID))) {
			_texts[textID] = FF7String();
			setModified(true);
		}
	}
}

void Section1File::autosizeTextWindows()
{
	QSet<quint8> textIDs = listUsedTexts();
	for (quint8 textID : textIDs) {
		if (textID >= _texts.size()) {
			continue;
		}
		QList<FF7Window> windows;
		listWindows(textID, windows);
		if (!windows.isEmpty()) {
			QSize size = FF7Font::calcSize(text(textID).data());
			for (FF7Window win : std::as_const(windows)) {
				if (win.displayType > 0) {
					continue; // TODO: estimate size for countdown and numerical display
				}
				win.w = quint16(size.width());
				win.h = quint16(size.height());
				QPoint pos = win.realPos();
				win.x = qint16(pos.x());
				win.y = qint16(pos.y());

				setWindow(win);
			}
		}
	}
}

//void Section1File::searchWindows() const
//{
//	int groupID=0;
//	for (const GrpScript &group : _grpScripts) {
//		const QList<Script> &scripts = group.scripts();
//		if (!scripts.isEmpty()) {
//			scripts.at(0)->searchWindows();
//			if (scripts.size() > 0) {
//				scripts.at(1)->searchWindows();

//				if (group.type() == GrpScript::Model) {
//					if (scripts.size() > 1) {
//						scripts.at(2)->searchWindows(); // talk
//					}
//					if (scripts.size() > 2) {
//						scripts.at(3)->searchWindows(); // touch
//					}
//				} else if (group.type() == GrpScript::Location) {
//					if (scripts.size() > 1) {
//						scripts.at(2)->searchWindows(); // talk
//					}
//					if (scripts.size() > 2) {
//						scripts.at(3)->searchWindows(); // touch
//					}
//					if (scripts.size() > 3) {
//						scripts.at(4)->searchWindows(); // move
//					}
//					if (scripts.size() > 4) {
//						scripts.at(5)->searchWindows(); // go
//					}
//					if (scripts.size() > 5) {
//						scripts.at(6)->searchWindows(); // go1
//					}
//					if (scripts.size() > 6) {
//						scripts.at(7)->searchWindows(); // leave
//					}
//				}

//			}
//		}
//	}
//		group.listWindows(groupID++, windows, text2win);
//}

const QList<FF7String> &Section1File::texts() const
{
	return _texts;
}

qsizetype Section1File::textCount() const
{
	return _texts.size();
}

const FF7String &Section1File::text(int textID) const
{
	return _texts.at(textID);
}

void Section1File::setText(int textID, const FF7String &text)
{
	if (textID >= 0 && textID < _texts.size()) {
		_texts.replace(textID, text);
		setModified(true);
	}
}

bool Section1File::insertText(int textID, const FF7String &text)
{
	if (textCount() < maxTextCount()) {
		_texts.insert(textID, text);
		for (GrpScript &grpScript : _grpScripts) {
			grpScript.shiftTextIds(textID - 1, +1);
		}
		setModified(true);
		return true;
	}
	return false;
}

void Section1File::deleteText(int textID)
{
	if (textID >= 0 && textID < _texts.size()) {
		_texts.removeAt(textID);
		for (GrpScript &grpScript : _grpScripts) {
			grpScript.shiftTextIds(textID, -1);
		}
		setModified(true);
	}
}

void Section1File::clearTexts()
{
	if (!_texts.isEmpty()) {
		_texts.clear();
		setModified(true);
	}
}

QSet<quint8> Section1File::listUsedTexts() const
{
	QSet<quint8> usedTexts;
	for (const GrpScript &grpScript : _grpScripts) {
		grpScript.listUsedTexts(usedTexts);
	}
	return usedTexts;
}

void Section1File::shiftTutIds(int row, int shift)
{
	for (GrpScript &grpScript : _grpScripts) {
		grpScript.shiftTutIds(row, shift);
	}
	setModified(true);
}

void Section1File::shiftPalIds(int row, int shift)
{
	for (GrpScript &grpScript : _grpScripts) {
		grpScript.shiftPalIds(row, shift);
	}
	setModified(true);
}

QSet<quint8> Section1File::listUsedTuts() const
{
	QSet<quint8> usedTuts;
	for (const GrpScript &grpScript : _grpScripts) {
		grpScript.listUsedTuts(usedTuts);
	}
	return usedTuts;
}

const QString &Section1File::author() const
{
	return _author;
}

void Section1File::setAuthor(const QString &author)
{
	_author = author;
	setModified(true);
}

quint16 Section1File::scale() const
{
	return _scale;
}

void Section1File::setScale(quint16 scale)
{
	_scale = scale;
	setModified(true);
}

quint16 Section1File::version() const
{
	return _version;
}

qsizetype Section1File::availableBytesForScripts() const
{
	TutFileStandard *tut = field()->tutosAndSounds();
	int AKAOCount = tut && tut->isOpen() ? tut->size() : 0; // TODO: opens tut
	return 65535 - (32 + grpScriptCount() * 72 + AKAOCount * 4);
}
