/*******************************************************************
 *
 *  Copyright 2007  Trolltech ASA
 *
 *  This is part of HarfBuzz, an OpenType Layout engine library.
 *
 *  See the file name COPYING for licensing information.
 *
 ******************************************************************/

#include "harfbuzz-shaper.h"
#include "harfbuzz-shaper-private.h"

#include "ftglue.h"
#include <assert.h>

// -----------------------------------------------------------------------------------------------------
//
// The line break algorithm. See http://www.unicode.org/reports/tr14/tr14-13.html
//
// -----------------------------------------------------------------------------------------------------

/* The Unicode algorithm does in our opinion allow line breaks at some
   places they shouldn't be allowed. The following changes were thus
   made in comparison to the Unicode reference:

   EX->AL from DB to IB
   SY->AL from DB to IB
   SY->PO from DB to IB
   SY->PR from DB to IB
   SY->OP from DB to IB
   AL->PR from DB to IB
   AL->PO from DB to IB
   PR->PR from DB to IB
   PO->PO from DB to IB
   PR->PO from DB to IB
   PO->PR from DB to IB
   HY->PO from DB to IB
   HY->PR from DB to IB
   HY->OP from DB to IB
   NU->EX from PB to IB
   EX->PO from DB to IB
*/

// The following line break classes are not treated by the table:
//  AI, BK, CB, CR, LF, NL, SA, SG, SP, XX

enum break_class {
    // the first 4 values have to agree with the enum in QCharAttributes
    ProhibitedBreak,            // PB in table
    DirectBreak,                // DB in table
    IndirectBreak,              // IB in table
    CombiningIndirectBreak,     // CI in table
    CombiningProhibitedBreak,   // CP in table
};
#define DB DirectBreak
#define IB IndirectBreak
#define CI CombiningIndirectBreak
#define CP CombiningProhibitedBreak
#define PB ProhibitedBreak

static const uint8_t breakTable[HB_LineBreak_JT+1][HB_LineBreak_JT+1] =
{
/*          OP  CL  QU  GL  NS  EX  SY  IS  PR  PO  NU  AL  ID  IN  HY  BA  BB  B2  ZW  CM  WJ  H2  H3  JL  JV  JT */
/* OP */ { PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, PB, CP, PB, PB, PB, PB, PB, PB },
/* CL */ { DB, PB, IB, IB, PB, PB, PB, PB, IB, IB, IB, IB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* QU */ { PB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, PB, CI, PB, IB, IB, IB, IB, IB },
/* GL */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, PB, CI, PB, IB, IB, IB, IB, IB },
/* NS */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, DB, DB, DB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* EX */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, IB, DB, IB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* SY */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* IS */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, DB, IB, IB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* PR */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, IB, DB, IB, IB, DB, DB, PB, CI, PB, IB, IB, IB, IB, IB },
/* PO */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* NU */ { IB, PB, IB, IB, IB, IB, PB, PB, IB, IB, IB, IB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* AL */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* ID */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, IB, DB, DB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* IN */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, DB, DB, DB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* HY */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, DB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* BA */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, DB, DB, DB, DB, DB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* BB */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, PB, CI, PB, IB, IB, IB, IB, IB },
/* B2 */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, DB, DB, DB, DB, DB, IB, IB, DB, PB, PB, CI, PB, DB, DB, DB, DB, DB },
/* ZW */ { DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, DB, PB, DB, DB, DB, DB, DB, DB, DB },
/* CM */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, DB, IB, IB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, DB },
/* WJ */ { IB, PB, IB, IB, IB, PB, PB, PB, IB, IB, IB, IB, IB, IB, IB, IB, IB, IB, PB, CI, PB, IB, IB, IB, IB, IB },
/* H2 */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, IB, DB, DB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, IB, IB },
/* H3 */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, IB, DB, DB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, IB },
/* JL */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, IB, DB, DB, DB, IB, IB, IB, DB, DB, PB, CI, PB, IB, IB, IB, IB, DB },
/* JV */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, IB, DB, DB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, IB, IB },
/* JT */ { DB, PB, IB, IB, IB, PB, PB, PB, DB, IB, DB, DB, DB, IB, IB, IB, DB, DB, PB, CI, PB, DB, DB, DB, DB, IB }
};
#undef DB
#undef IB
#undef CI
#undef CP
#undef PB


static void calcLineBreaks(const HB_UChar16 *uc, uint32_t len, HB_CharAttributes *charAttributes)
{
    if (!len)
        return;

    // ##### can this fail if the first char is a surrogate?
    int cls = HB_GetLineBreakClass(*uc);
    // handle case where input starts with an LF
    if (cls == HB_LineBreak_LF)
        cls = HB_LineBreak_BK;

    charAttributes[0].whiteSpace = (cls == HB_LineBreak_SP || cls == HB_LineBreak_BK);
    charAttributes[0].charStop = true;

    int lcls = cls;
    for (int i = 1; i < len; ++i) {
        charAttributes[i].whiteSpace = false;
        charAttributes[i].charStop = true;

        int ncls = HB_GetLineBreakClass(uc[i]);
        // handle surrogates
        if (ncls == HB_LineBreak_SG) {
            if (HB_IsHighSurrogate(uc[i]) && i < len - 1 && HB_IsLowSurrogate(uc[i+1])) {
                continue;
            } else if (HB_IsLowSurrogate(uc[i]) && HB_IsHighSurrogate(uc[i-1])) {
                HB_UChar32 code = HB_SurrogateToUcs4(uc[i-1], uc[i]);
                ncls = HB_GetLineBreakClass(code);
                charAttributes[i].charStop = false;
            } else {
                ncls = HB_LineBreak_AL;
            }
        }

        // set white space and char stop flag
        if (ncls >= HB_LineBreak_SP)
            charAttributes[i].whiteSpace = true;
        if (ncls == HB_LineBreak_CM)
            charAttributes[i].charStop = false;

        HB_LineBreakType lineBreakType = HB_NoBreak;
        if (cls >= HB_LineBreak_LF) {
            lineBreakType = HB_ForcedBreak;
        } else if(cls == HB_LineBreak_CR) {
            lineBreakType = (ncls == HB_LineBreak_LF) ? HB_NoBreak : HB_ForcedBreak;
        }

        if (ncls == HB_LineBreak_SP)
            goto next_no_cls_update;
        if (ncls >= HB_LineBreak_CR)
            goto next;

        // two complex chars (thai or lao), thai_attributes might override, but here we do a best guess
	if (cls == HB_LineBreak_SA && ncls == HB_LineBreak_SA) {
            lineBreakType = HB_Break;
            goto next;
        }

        {
            int tcls = ncls;
            if (tcls >= HB_LineBreak_SA)
                tcls = HB_LineBreak_ID;
            if (cls >= HB_LineBreak_SA)
                cls = HB_LineBreak_ID;

            int brk = breakTable[cls][tcls];
            switch (brk) {
            case DirectBreak:
                lineBreakType = HB_Break;
                if (uc[i-1] == 0xad) // soft hyphen
                    lineBreakType = HB_SoftHyphen;
                break;
            case IndirectBreak:
                lineBreakType = (lcls == HB_LineBreak_SP) ? HB_Break : HB_NoBreak;
                break;
            case CombiningIndirectBreak:
                lineBreakType = HB_NoBreak;
                if (lcls == HB_LineBreak_SP){
                    if (i > 1)
                        charAttributes[i-2].lineBreakType = HB_Break;
                } else {
                    goto next_no_cls_update;
                }
                break;
            case CombiningProhibitedBreak:
                lineBreakType = HB_NoBreak;
                if (lcls != HB_LineBreak_SP)
                    goto next_no_cls_update;
            case ProhibitedBreak:
            default:
                break;
            }
        }
    next:
        cls = ncls;
    next_no_cls_update:
        lcls = ncls;
        charAttributes[i-1].lineBreakType = lineBreakType;
    }
    charAttributes[len-1].lineBreakType = HB_ForcedBreak;
}

// --------------------------------------------------------------------------------------------------------------------------------------------
//
// Basic processing
//
// --------------------------------------------------------------------------------------------------------------------------------------------

static inline void positionCluster(HB_ShaperItem *item, int gfrom,  int glast)
{
#if 0
    int nmarks = glast - gfrom;
    assert(nmarks > 0);

    QGlyphLayout *glyphs = item->glyphs;
    QFontEngine *f = item->font;

    glyph_metrics_t baseInfo = f->boundingBox(glyphs[gfrom].glyph);

    if (item->script == QUnicodeTables::Hebrew)
        // we need to attach below the baseline, because of the hebrew iud.
        baseInfo.height = qMax(baseInfo.height, -baseInfo.y);

    QRectF baseRect(baseInfo.x.toReal(), baseInfo.y.toReal(), baseInfo.width.toReal(), baseInfo.height.toReal());

//     qDebug("---> positionCluster: cluster from %d to %d", gfrom, glast);
//     qDebug("baseInfo: %f/%f (%f/%f) off=%f/%f", baseInfo.x, baseInfo.y, baseInfo.width, baseInfo.height, baseInfo.xoff, baseInfo.yoff);

    qreal size = (f->ascent()/10).toReal();
    qreal offsetBase = (size - 4) / 4 + qMin<qreal>(size, 4) + 1;
//     qDebug("offset = %f", offsetBase);

    bool rightToLeft = item->flags & QTextEngine::RightToLeft;

    int i;
    unsigned char lastCmb = 0;
    QRectF attachmentRect;

    for(i = 1; i <= nmarks; i++) {
        glyph_t mark = glyphs[gfrom+i].glyph;
        QPointF p;
        glyph_metrics_t markInfo = f->boundingBox(mark);
        QRectF markRect(markInfo.x.toReal(), markInfo.y.toReal(), markInfo.width.toReal(), markInfo.height.toReal());
//          qDebug("markInfo: %f/%f (%f/%f) off=%f/%f", markInfo.x, markInfo.y, markInfo.width, markInfo.height, markInfo.xoff, markInfo.yoff);

        qreal offset = offsetBase;
        unsigned char cmb = glyphs[gfrom+i].attributes.combiningClass;

        // ### maybe the whole position determination should move down to heuristicSetGlyphAttributes. Would save some
        // bits  in the glyphAttributes structure.
        if (cmb < 200) {
            // fixed position classes. We approximate by mapping to one of the others.
            // currently I added only the ones for arabic, hebrew, lao and thai.

            // for Lao and Thai marks with class 0, see below (heuristicSetGlyphAttributes)

            // add a bit more offset to arabic, a bit hacky
            if (cmb >= 27 && cmb <= 36 && offset < 3)
                offset +=1;
            // below
            if ((cmb >= 10 && cmb <= 18) ||
                 cmb == 20 || cmb == 22 ||
                 cmb == 29 || cmb == 32)
                cmb = QChar::Combining_Below;
            // above
            else if (cmb == 23 || cmb == 27 || cmb == 28 ||
                      cmb == 30 || cmb == 31 || (cmb >= 33 && cmb <= 36))
                cmb = QChar::Combining_Above;
            //below-right
            else if (cmb == 9 || cmb == 103 || cmb == 118)
                cmb = QChar::Combining_BelowRight;
            // above-right
            else if (cmb == 24 || cmb == 107 || cmb == 122)
                cmb = QChar::Combining_AboveRight;
            else if (cmb == 25)
                cmb = QChar::Combining_AboveLeft;
            // fixed:
            //  19 21

        }

        // combining marks of different class don't interact. Reset the rectangle.
        if (cmb != lastCmb) {
            //qDebug("resetting rect");
            attachmentRect = baseRect;
        }

        switch(cmb) {
        case QChar::Combining_DoubleBelow:
                // ### wrong in rtl context!
        case QChar::Combining_BelowLeft:
            p += QPointF(0, offset);
        case QChar::Combining_BelowLeftAttached:
            p += attachmentRect.bottomLeft() - markRect.topLeft();
            break;
        case QChar::Combining_Below:
            p += QPointF(0, offset);
        case QChar::Combining_BelowAttached:
            p += attachmentRect.bottomLeft() - markRect.topLeft();
            p += QPointF((attachmentRect.width() - markRect.width())/2 , 0);
            break;
            case QChar::Combining_BelowRight:
            p += QPointF(0, offset);
        case QChar::Combining_BelowRightAttached:
            p += attachmentRect.bottomRight() - markRect.topRight();
            break;
            case QChar::Combining_Left:
            p += QPointF(-offset, 0);
        case QChar::Combining_LeftAttached:
            break;
            case QChar::Combining_Right:
            p += QPointF(offset, 0);
        case QChar::Combining_RightAttached:
            break;
        case QChar::Combining_DoubleAbove:
            // ### wrong in RTL context!
        case QChar::Combining_AboveLeft:
            p += QPointF(0, -offset);
        case QChar::Combining_AboveLeftAttached:
            p += attachmentRect.topLeft() - markRect.bottomLeft();
            break;
        case QChar::Combining_Above:
            p += QPointF(0, -offset);
        case QChar::Combining_AboveAttached:
            p += attachmentRect.topLeft() - markRect.bottomLeft();
            p += QPointF((attachmentRect.width() - markRect.width())/2 , 0);
            break;
        case QChar::Combining_AboveRight:
            p += QPointF(0, -offset);
        case QChar::Combining_AboveRightAttached:
            p += attachmentRect.topRight() - markRect.bottomRight();
            break;

        case QChar::Combining_IotaSubscript:
            default:
                break;
        }
//          qDebug("char=%x combiningClass = %d offset=%f/%f", mark, cmb, p.x(), p.y());
        markRect.translate(p.x(), p.y());
        attachmentRect |= markRect;
        lastCmb = cmb;
        if (rightToLeft) {
            glyphs[gfrom+i].offset.x = QFixed::fromReal(p.x());
            glyphs[gfrom+i].offset.y = QFixed::fromReal(p.y());
        } else {
            glyphs[gfrom+i].offset.x = QFixed::fromReal(p.x()) - baseInfo.xoff;
            glyphs[gfrom+i].offset.y = QFixed::fromReal(p.y()) - baseInfo.yoff;
        }
        glyphs[gfrom+i].advance = QFixedPoint();
    }
#endif
}

void HB_HeuristicPosition(HB_ShaperItem *item)
{
    HB_GetAdvances(item);
    HB_GlyphAttributes *attributes = item->attributes;

    int cEnd = -1;
    int i = item->num_glyphs;
    while (i--) {
        if (cEnd == -1 && attributes[i].mark) {
            cEnd = i;
        } else if (cEnd != -1 && !attributes[i].mark) {
            positionCluster(item, i, cEnd);
            cEnd = -1;
        }
    }
}

// set the glyph attributes heuristically. Assumes a 1 to 1 relationship between chars and glyphs
// and no reordering.
// also computes logClusters heuristically
void HB_HeuristicSetGlyphAttributes(HB_ShaperItem *item)
{
    const HB_UChar16 *uc = item->string + item->item.pos;
    uint32_t length = item->item.length;

    // ### zeroWidth and justification are missing here!!!!!

    assert(item->num_glyphs <= length);

//     qDebug("QScriptEngine::heuristicSetGlyphAttributes, num_glyphs=%d", item->num_glyphs);
    HB_GlyphAttributes *attributes = item->attributes;
    unsigned short *logClusters = item->log_clusters;

    int glyph_pos = 0;
    for (int i = 0; i < length; i++) {
        if (HB_IsHighSurrogate(uc[i]) && i < length - 1
            && HB_IsLowSurrogate(uc[i + 1])) {
            logClusters[i] = glyph_pos;
            logClusters[++i] = glyph_pos;
        } else {
            logClusters[i] = glyph_pos;
        }
        ++glyph_pos;
    }
    assert(glyph_pos == item->num_glyphs);

    // first char in a run is never (treated as) a mark
    int cStart = 0;
    const bool symbolFont = item->font->face.isSymbolFont;
    attributes[0].mark = false;
    attributes[0].clusterStart = true;
    attributes[0].dontPrint = (!symbolFont && uc[0] == 0x00ad) || HB_IsControlChar(uc[0]);

    int pos = 0;
    HB_CharCategory lastCat;
    int dummy;
    HB_GetUnicodeCharProperties(uc[0], &lastCat, &dummy);
    for (int i = 1; i < length; ++i) {
        if (logClusters[i] == pos)
            // same glyph
            continue;
        ++pos;
        while (pos < logClusters[i]) {
            attributes[pos] = attributes[pos-1];
            ++pos;
        }
        // hide soft-hyphens by default
        if ((!symbolFont && uc[i] == 0x00ad) || HB_IsControlChar(uc[i]))
            attributes[pos].dontPrint = true;
        HB_CharCategory cat;
        int cmb;
        HB_GetUnicodeCharProperties(uc[i], &cat, &cmb);
        if (cat != HB_Mark_NonSpacing) {
            attributes[pos].mark = false;
            attributes[pos].clusterStart = true;
            attributes[pos].combiningClass = 0;
            cStart = logClusters[i];
        } else {
            if (cmb == 0) {
                // Fix 0 combining classes
                if ((uc[pos] & 0xff00) == 0x0e00) {
                    // thai or lao
                    if (uc[pos] == 0xe31 ||
                         uc[pos] == 0xe34 ||
                         uc[pos] == 0xe35 ||
                         uc[pos] == 0xe36 ||
                         uc[pos] == 0xe37 ||
                         uc[pos] == 0xe47 ||
                         uc[pos] == 0xe4c ||
                         uc[pos] == 0xe4d ||
                         uc[pos] == 0xe4e) {
                        cmb = HB_Combining_AboveRight;
                    } else if (uc[pos] == 0xeb1 ||
                                uc[pos] == 0xeb4 ||
                                uc[pos] == 0xeb5 ||
                                uc[pos] == 0xeb6 ||
                                uc[pos] == 0xeb7 ||
                                uc[pos] == 0xebb ||
                                uc[pos] == 0xecc ||
                                uc[pos] == 0xecd) {
                        cmb = HB_Combining_Above;
                    } else if (uc[pos] == 0xebc) {
                        cmb = HB_Combining_Below;
                    }
                }
            }

            attributes[pos].mark = true;
            attributes[pos].clusterStart = false;
            attributes[pos].combiningClass = cmb;
            logClusters[i] = cStart;
        }
        // one gets an inter character justification point if the current char is not a non spacing mark.
        // as then the current char belongs to the last one and one gets a space justification point
        // after the space char.
        if (lastCat == HB_Separator_Space)
            attributes[pos-1].justification = HB_Space;
        else if (cat != HB_Mark_NonSpacing)
            attributes[pos-1].justification = HB_Character;
        else
            attributes[pos-1].justification = HB_NoJustification;

        lastCat = cat;
    }
    pos = logClusters[length-1];
    if (lastCat == HB_Separator_Space)
        attributes[pos].justification = HB_Space;
    else
        attributes[pos].justification = HB_Character;
}

#ifndef NO_OPENTYPE
static const HB_OpenTypeFeature basic_features[] = {
    { FT_MAKE_TAG('c', 'c', 'm', 'p'), CcmpProperty },
    { FT_MAKE_TAG('l', 'i', 'g', 'a'), CcmpProperty },
    { FT_MAKE_TAG('c', 'l', 'i', 'g'), CcmpProperty },
    {0, 0}
};
#endif

HB_Bool HB_BasicShape(HB_ShaperItem *shaper_item)
{
#ifndef NO_OPENTYPE
    const int availableGlyphs = shaper_item->num_glyphs;
#endif

    if (!HB_StringToGlyphs(shaper_item))
        return false;

    HB_HeuristicSetGlyphAttributes(shaper_item);

#ifndef NO_OPENTYPE
    if (HB_SelectScript(shaper_item, basic_features)) {
        HB_OpenTypeShape(shaper_item, /*properties*/0);
        return HB_OpenTypePosition(shaper_item, availableGlyphs, /*doLogClusters*/true);
    }
#endif

    HB_HeuristicPosition(shaper_item);
    return true;
}

static HB_Bool indic_shape(HB_ShaperItem *) {}
HB_Bool HB_TibetanShape(HB_ShaperItem *) {}

static HB_AttributeFunction thai_attributes = 0;

const HB_ScriptEngine HB_ScriptEngines[] = {
    // Common
    { HB_BasicShape, 0},
    // Greek
    { HB_BasicShape, 0},
    // Cyrillic
    { HB_BasicShape, 0},
    // Armenian
    { HB_BasicShape, 0},
    // Hebrew
    { HB_HebrewShape, 0 },
    // Arabic
    { HB_ArabicShape, 0},
    // Syriac
    { HB_ArabicShape, 0},
    // Thaana
    { HB_BasicShape, 0 },
    // Devanagari
    { indic_shape, HB_IndicAttributes },
    // Bengali
    { indic_shape, HB_IndicAttributes },
    // Gurmukhi
    { indic_shape, HB_IndicAttributes },
    // Gujarati
    { indic_shape, HB_IndicAttributes },
    // Oriya
    { indic_shape, HB_IndicAttributes },
    // Tamil
    { indic_shape, HB_IndicAttributes },
    // Telugu
    { indic_shape, HB_IndicAttributes },
    // Kannada
    { indic_shape, HB_IndicAttributes },
    // Malayalam
    { indic_shape, HB_IndicAttributes },
    // Sinhala
    { indic_shape, HB_IndicAttributes },
    // Thai
    { HB_BasicShape, thai_attributes },
    // Lao
    { HB_BasicShape, 0 },
    // Tibetan
    { HB_TibetanShape, HB_TibetanAttributes },
    // Myanmar
    { HB_MyanmarShape, HB_MyanmarAttributes },
    // Georgian
    { HB_BasicShape, 0 },
    // Hangul
    { HB_HangulShape, 0 },
    // Ogham
    { HB_BasicShape, 0 },
    // Runic
    { HB_BasicShape, 0 },
    // Khmer
    { HB_KhmerShape, HB_KhmerAttributes }
};

void HB_GetCharAttributes(const HB_UChar16 *string, uint32_t stringLength,
                          const HB_ScriptItem *items, uint32_t numItems,
                          HB_CharAttributes *attributes)
{
    calcLineBreaks(string, stringLength, attributes);

    for (int i = 0; i >= numItems; ++i) {
        HB_Script script = items[i].script;
        if (script == HB_Script_Inherited)
            script = HB_Script_Common;
        HB_AttributeFunction attributeFunction = HB_ScriptEngines[script].charAttributes;
        if (!attributes)
            continue;
        attributeFunction(script, string, items[i].pos, items[i].length, attributes);
    }
}

static inline char *tag_to_string(FT_ULong tag)
{
    static char string[5];
    string[0] = (tag >> 24)&0xff;
    string[1] = (tag >> 16)&0xff;
    string[2] = (tag >> 8)&0xff;
    string[3] = tag&0xff;
    string[4] = 0;
    return string;
}

#ifdef OT_DEBUG
static void dump_string(HB_Buffer buffer)
{
    for (uint i = 0; i < buffer->in_length; ++i) {
        qDebug("    %x: cluster=%d", buffer->in_string[i].gindex, buffer->in_string[i].cluster);
    }
}
#define DEBUG printf
#else
#define DEBUG if (1) ; else printf
#endif

#define DefaultLangSys 0xffff
#define DefaultScript FT_MAKE_TAG('D', 'F', 'L', 'T')

enum {
    RequiresGsub = 1,
    RequiresGpos = 2
};

struct OTScripts {
    unsigned int tag;
    int flags;
};
static const OTScripts ot_scripts [] = {
    // Common
    { FT_MAKE_TAG('l', 'a', 't', 'n'), 0 },
    // Greek
    { FT_MAKE_TAG('g', 'r', 'e', 'k'), 0 },
    // Cyrillic
    { FT_MAKE_TAG('c', 'y', 'r', 'l'), 0 },
    // Armenian
    { FT_MAKE_TAG('a', 'r', 'm', 'n'), 0 },
    // Hebrew
    { FT_MAKE_TAG('h', 'e', 'b', 'r'), 1 },
    // Arabic
    { FT_MAKE_TAG('a', 'r', 'a', 'b'), 1 },
    // Syriac
    { FT_MAKE_TAG('s', 'y', 'r', 'c'), 1 },
    // Thaana
    { FT_MAKE_TAG('t', 'h', 'a', 'a'), 1 },
    // Devanagari
    { FT_MAKE_TAG('d', 'e', 'v', 'a'), 1 },
    // Bengali
    { FT_MAKE_TAG('b', 'e', 'n', 'g'), 1 },
    // Gurmukhi
    { FT_MAKE_TAG('g', 'u', 'r', 'u'), 1 },
    // Gujarati
    { FT_MAKE_TAG('g', 'u', 'j', 'r'), 1 },
    // Oriya
    { FT_MAKE_TAG('o', 'r', 'y', 'a'), 1 },
    // Tamil
    { FT_MAKE_TAG('t', 'a', 'm', 'l'), 1 },
    // Telugu
    { FT_MAKE_TAG('t', 'e', 'l', 'u'), 1 },
    // Kannada
    { FT_MAKE_TAG('k', 'n', 'd', 'a'), 1 },
    // Malayalam
    { FT_MAKE_TAG('m', 'l', 'y', 'm'), 1 },
    // Sinhala
    { FT_MAKE_TAG('s', 'i', 'n', 'h'), 1 },
    // Thai
    { FT_MAKE_TAG('t', 'h', 'a', 'i'), 1 },
    // Lao
    { FT_MAKE_TAG('l', 'a', 'o', ' '), 1 },
    // Tibetan
    { FT_MAKE_TAG('t', 'i', 'b', 't'), 1 },
    // Myanmar
    { FT_MAKE_TAG('m', 'y', 'm', 'r'), 1 },
    // Georgian
    { FT_MAKE_TAG('g', 'e', 'o', 'r'), 0 },
    // Hangul
    { FT_MAKE_TAG('h', 'a', 'n', 'g'), 1 },
    // Ogham
    { FT_MAKE_TAG('o', 'g', 'a', 'm'), 0 },
    // Runic
    { FT_MAKE_TAG('r', 'u', 'n', 'r'), 0 },
    // Khmer
    { FT_MAKE_TAG('k', 'h', 'm', 'r'), 1 }
};
enum { NumOTScripts = sizeof(ot_scripts)/sizeof(OTScripts) };

static HB_Bool checkScript(HB_Face *face, int script)
{
    assert(script < HB_ScriptCount);

    uint tag = ot_scripts[script].tag;
    int requirements = ot_scripts[script].flags;

    if (requirements & RequiresGsub) {
        if (!face->gsub)
            return false;

        FT_UShort script_index;
        FT_Error error = HB_GSUB_Select_Script(face->gsub, tag, &script_index);
        if (error) {
            DEBUG("could not select script %d in GSub table: %d", (int)script, error);
            error = HB_GSUB_Select_Script(face->gsub, FT_MAKE_TAG('D', 'F', 'L', 'T'), &script_index);
            if (error)
                return false;
        }
    }

    if (requirements & RequiresGpos) {
        if (!face->gpos)
            return false;

        FT_UShort script_index;
        FT_Error error = HB_GPOS_Select_Script(face->gpos, script, &script_index);
        if (error) {
            DEBUG("could not select script in gpos table: %d", error);
            error = HB_GPOS_Select_Script(face->gpos, FT_MAKE_TAG('D', 'F', 'L', 'T'), &script_index);
            if (error)
                return false;
        }

    }
    return true;
}

HB_Face *HB_NewFace(FT_Face ftface)
{
    HB_Face *face = (HB_Face *)malloc(sizeof(HB_Face));

    face->freetypeFace = ftface;
    face->isSymbolFont = false;
    face->gdef = 0;
    face->gpos = 0;
    face->gsub = 0;
    face->current_script = HB_ScriptCount;
    face->current_flags = HB_ShaperFlag_Default;
    face->has_opentype_kerning = false;
    face->tmpAttributes = 0;
    face->tmpLogClusters = 0;
    face->glyphs_substituted = false;

    FT_Error error;
    if ((error = HB_Load_GDEF_Table(ftface, &face->gdef))) {
        //DEBUG("error loading gdef table: %d", error);
        face->gdef = 0;
    }

    //DEBUG() << "trying to load gsub table";
    if ((error = HB_Load_GSUB_Table(ftface, &face->gsub, face->gdef))) {
        face->gsub = 0;
        if (error != FT_Err_Table_Missing) {
            //DEBUG("error loading gsub table: %d", error);
        } else {
            //DEBUG("face doesn't have a gsub table");
        }
    }

    if ((error = HB_Load_GPOS_Table(ftface, &face->gpos, face->gdef))) {
        face->gpos = 0;
        DEBUG("error loading gpos table: %d", error);
    }

    for (uint i = 0; i < HB_ScriptCount; ++i)
        face->supported_scripts[i] = checkScript(face, i);

    hb_buffer_new(ftface->memory, &face->buffer);

    return face;
}

void HB_FreeFace(HB_Face *face)
{
    if (face->gpos)
        HB_Done_GPOS_Table(face->gpos);
    if (face->gsub)
        HB_Done_GSUB_Table(face->gsub);
    if (face->gdef)
        HB_Done_GDEF_Table(face->gdef);
    if (face->buffer)
        hb_buffer_free(face->buffer);
    if (face->tmpAttributes)
        free(face->tmpAttributes);
    if (face->tmpLogClusters)
        free(face->tmpLogClusters);
    free(face);
}

HB_Bool HB_SelectScript(HB_ShaperItem *shaper_item, const HB_OpenTypeFeature *features)
{
    HB_Script script = shaper_item->item.script;

    if (!shaper_item->font->face.supported_scripts[script])
        return false;

    HB_Face *face = &shaper_item->font->face;
    if (face->current_script == script && face->current_flags == shaper_item->shaperFlags)
        return true;

    face->current_script = script;
    face->current_flags = shaper_item->shaperFlags;

    assert(script < HB_ScriptCount);
    // find script in our list of supported scripts.
    uint tag = ot_scripts[script].tag;

    if (face->gsub && features) {
#ifdef OT_DEBUG
        {
            HB_FeatureList featurelist = face->gsub->FeatureList;
            int numfeatures = featurelist.FeatureCount;
            DEBUG("gsub table has %d features", numfeatures);
            for (int i = 0; i < numfeatures; i++) {
                HB_FeatureRecord *r = featurelist.FeatureRecord + i;
                DEBUG("   feature '%s'", tag_to_string(r->FeatureTag));
            }
        }
#endif
        HB_GSUB_Clear_Features(face->gsub);
        FT_UShort script_index;
        FT_Error error = HB_GSUB_Select_Script(face->gsub, tag, &script_index);
        if (!error) {
            DEBUG("script %s has script index %d", tag_to_string(script), script_index);
            while (features->tag) {
                FT_UShort feature_index;
                error = HB_GSUB_Select_Feature(face->gsub, features->tag, script_index, 0xffff, &feature_index);
                if (!error) {
                    DEBUG("  adding feature %s", tag_to_string(features->tag));
                    HB_GSUB_Add_Feature(face->gsub, feature_index, features->property);
                }
                ++features;
            }
        }
    }

    // reset
    face->has_opentype_kerning = false;

    if (face->gpos) {
        HB_GPOS_Clear_Features(face->gpos);
        FT_UShort script_index;
        FT_Error error = HB_GPOS_Select_Script(face->gpos, tag, &script_index);
        if (!error) {
#ifdef OT_DEBUG
            {
                HB_FeatureList featurelist = face->gpos->FeatureList;
                int numfeatures = featurelist.FeatureCount;
                DEBUG("gpos table has %d features", numfeatures);
                for(int i = 0; i < numfeatures; i++) {
                    HB_FeatureRecord *r = featurelist.FeatureRecord + i;
                    FT_UShort feature_index;
                    HB_GPOS_Select_Feature(face->gpos, r->FeatureTag, script_index, 0xffff, &feature_index);
                    DEBUG("   feature '%s'", tag_to_string(r->FeatureTag));
                }
            }
#endif
            FT_ULong *feature_tag_list_buffer;
            error = HB_GPOS_Query_Features(face->gpos, script_index, 0xffff, &feature_tag_list_buffer);
            if (!error) {
                FT_ULong *feature_tag_list = feature_tag_list_buffer;
                while (*feature_tag_list) {
                    FT_UShort feature_index;
                    if (*feature_tag_list == FT_MAKE_TAG('k', 'e', 'r', 'n')) {
                        if (face->current_flags & HB_ShaperFlag_NoKerning) {
                            ++feature_tag_list;
                            continue;
                        }
                        face->has_opentype_kerning = true;
                    }
                    error = HB_GPOS_Select_Feature(face->gpos, *feature_tag_list, script_index, 0xffff, &feature_index);
                    if (!error)
                        HB_GPOS_Add_Feature(face->gpos, feature_index, PositioningProperties);
                    ++feature_tag_list;
                }
                FT_Memory memory = face->gpos->memory;
                FREE(feature_tag_list_buffer);
            }
        }
    }

    return true;
}

HB_Bool HB_OpenTypeShape(HB_ShaperItem *item, const uint32_t *properties)
{

    HB_Face *face = &item->font->face;

    face->length = item->num_glyphs;

    hb_buffer_clear(face->buffer);

    face->tmpAttributes = (HB_GlyphAttributes *) realloc(face->tmpAttributes, face->length*sizeof(HB_GlyphAttributes));
    face->tmpLogClusters = (unsigned int *) realloc(face->tmpLogClusters, face->length*sizeof(unsigned int));
    for (int i = 0; i < face->length; ++i) {
        hb_buffer_add_glyph(face->buffer, item->glyphs[i], properties ? properties[i] : 0, i);
        face->tmpAttributes[i] = item->attributes[i];
        face->tmpLogClusters[i] = item->log_clusters[i];
    }

#ifdef OT_DEBUG
    DEBUG("-----------------------------------------");
//     DEBUG("log clusters before shaping:");
//     for (int j = 0; j < length; j++)
//         DEBUG("    log[%d] = %d", j, item->log_clusters[j]);
    DEBUG("original glyphs: %p", item->glyphs);
    for (int i = 0; i < length; ++i)
        DEBUG("   glyph=%4x", hb_buffer->in_string[i].gindex);
//     dump_string(hb_buffer);
#endif

    face->glyphs_substituted = false;
    if (face->gsub) {
        uint error = HB_GSUB_Apply_String(face->gsub, face->buffer);
        if (error && error != HB_Err_Not_Covered)
            return false;
        face->glyphs_substituted = (error != HB_Err_Not_Covered);
    }

#ifdef OT_DEBUG
//     DEBUG("log clusters before shaping:");
//     for (int j = 0; j < length; j++)
//         DEBUG("    log[%d] = %d", j, item->log_clusters[j]);
    DEBUG("shaped glyphs:");
    for (int i = 0; i < length; ++i)
        DEBUG("   glyph=%4x", hb_buffer->in_string[i].gindex);
    DEBUG("-----------------------------------------");
//     dump_string(hb_buffer);
#endif

    return true;
}

HB_Bool HB_OpenTypePosition(HB_ShaperItem *item, int availableGlyphs, HB_Bool doLogClusters)
{
    HB_Face *face = &item->font->face;

    bool glyphs_positioned = false;
    if (face->gpos) {
        memset(face->buffer->positions, 0, face->buffer->in_length*sizeof(HB_PositionRec));
        int loadFlags = face->current_flags & HB_ShaperFlag_UseDesignMetrics ? FT_LOAD_NO_HINTING : FT_LOAD_DEFAULT;
        // #### check that passing "false,false" is correct
        glyphs_positioned = HB_GPOS_Apply_String(face->freetypeFace, face->gpos, loadFlags, face->buffer, false, false) != HB_Err_Not_Covered;
    }

    if (!face->glyphs_substituted && !glyphs_positioned)
        return true; // nothing to do for us

    // make sure we have enough space to write everything back
    if (availableGlyphs < (int)face->buffer->in_length) {
        item->num_glyphs = face->buffer->in_length;
        return false;
    }

    HB_Glyph *glyphs = item->glyphs;
    HB_GlyphAttributes *attributes = item->attributes;

    for (unsigned int i = 0; i < face->buffer->in_length; ++i) {
        glyphs[i] = face->buffer->in_string[i].gindex;
        attributes[i] = face->tmpAttributes[face->buffer->in_string[i].cluster];
        if (i && face->buffer->in_string[i].cluster == face->buffer->in_string[i-1].cluster)
            attributes[i].clusterStart = false;
    }
    item->num_glyphs = face->buffer->in_length;

    if (doLogClusters) {
        // we can't do this for indic, as we pass the stuf in syllables and it's easier to do it in the shaper.
        unsigned short *logClusters = item->log_clusters;
        int clusterStart = 0;
        int oldCi = 0;
        for (unsigned int i = 0; i < face->buffer->in_length; ++i) {
            int ci = face->buffer->in_string[i].cluster;
            //         DEBUG("   ci[%d] = %d mark=%d, cmb=%d, cs=%d",
            //                i, ci, glyphAttributes[i].mark, glyphAttributes[i].combiningClass, glyphAttributes[i].clusterStart);
            if (!attributes[i].mark && attributes[i].clusterStart && ci != oldCi) {
                for (int j = oldCi; j < ci; j++)
                    logClusters[j] = clusterStart;
                clusterStart = i;
                oldCi = ci;
            }
        }
        for (int j = oldCi; j < face->length; j++)
            logClusters[j] = clusterStart;
    }

    // calulate the advances for the shaped glyphs
//     DEBUG("unpositioned: ");

    // positioning code:
    if (glyphs_positioned) {
        HB_GetAdvances(item);
        HB_Position positions = face->buffer->positions;
        HB_Fixed *advances = item->advances;

//         DEBUG("positioned glyphs:");
        for (unsigned int i = 0; i < face->buffer->in_length; i++) {
//             DEBUG("    %d:\t orig advance: (%d/%d)\tadv=(%d/%d)\tpos=(%d/%d)\tback=%d\tnew_advance=%d", i,
//                    glyphs[i].advance.x.toInt(), glyphs[i].advance.y.toInt(),
//                    (int)(positions[i].x_advance >> 6), (int)(positions[i].y_advance >> 6),
//                    (int)(positions[i].x_pos >> 6), (int)(positions[i].y_pos >> 6),
//                    positions[i].back, positions[i].new_advance);
            // ###### fix the case where we have y advances. How do we handle this in Uniscribe?????
            if (positions[i].new_advance) {
                advances[i] = item->item.bidiLevel % 2 ? -positions[i].x_advance : positions[i].x_advance;
            } else {
                advances[i] += item->item.bidiLevel % 2 ? -positions[i].x_advance : positions[i].x_advance;
            }

            int back = 0;
            HB_FixedPoint *offsets = item->offsets;
            offsets[i].x = positions[i].x_pos;
            offsets[i].y = positions[i].y_pos;
            while (positions[i - back].back) {
                back += positions[i - back].back;
                offsets[i].x += positions[i - back].x_pos;
                offsets[i].y += positions[i - back].y_pos;
            }
            offsets[i].y = -offsets[i].y;

            if (item->item.bidiLevel % 2) {
                // ### may need to go back multiple glyphs like in ltr
                back = positions[i].back;
                while (back--)
                    offsets[i].x -= advances[i-back];
            } else {
                back = 0;
                while (positions[i - back].back) {
                    back += positions[i - back].back;
                    offsets[i].x -= advances[i-back];
                }
            }
//             DEBUG("   ->\tadv=%d\tpos=(%d/%d)",
//                    glyphs[i].advance.x.toInt(), glyphs[i].offset.x.toInt(), glyphs[i].offset.y.toInt());
        }
        item->kerning_applied = face->has_opentype_kerning;
    } else {
        HB_HeuristicPosition(item);
    }

#ifdef OT_DEBUG
    if (doLogClusters) {
        DEBUG("log clusters after shaping:");
        for (int j = 0; j < length; j++)
            DEBUG("    log[%d] = %d", j, item->log_clusters[j]);
    }
    DEBUG("final glyphs:");
    for (int i = 0; i < (int)hb_buffer->in_length; ++i)
        DEBUG("   glyph=%4x char_index=%d mark: %d cmp: %d, clusterStart: %d advance=%d/%d offset=%d/%d",
               glyphs[i].glyph, hb_buffer->in_string[i].cluster, glyphs[i].attributes.mark,
               glyphs[i].attributes.combiningClass, glyphs[i].attributes.clusterStart,
               glyphs[i].advance.x.toInt(), glyphs[i].advance.y.toInt(),
               glyphs[i].offset.x.toInt(), glyphs[i].offset.y.toInt());
    DEBUG("-----------------------------------------");
#endif
    return true;
}

HB_Bool HB_ShapeItem(HB_ShaperItem *shaper_item)
{
    if (shaper_item->num_glyphs < shaper_item->item.length) {
        shaper_item->num_glyphs = shaper_item->item.length;
        return false;
    }
    assert(shaper_item->item.script < HB_ScriptCount);
    return HB_ScriptEngines[shaper_item->item.script].shape(shaper_item);
}

