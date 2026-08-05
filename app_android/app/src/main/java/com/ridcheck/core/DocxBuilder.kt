package com.ridcheck.core

import java.io.ByteArrayOutputStream
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream

/**
 * 零依赖 Word (.docx) 生成器：直接产出最小 OOXML 包（[Content_Types].xml / _rels / word/document.xml）。
 * 纯 JVM，可用 java.util.zip 单元测试。字号 sz 单位为半磅。中文用微软雅黑，Word/WPS 均可打开。
 */
object DocxBuilder {

    const val ACCENT = "5E35B1"      // 深紫（标题/表头）
    const val ACCENT_SOFT = "EDE7F6" // 浅紫（表头底）
    const val INK = "1A1A1A"
    const val GRAY = "666666"

    fun build(block: (Doc) -> Unit): ByteArray {
        val doc = Doc()
        block(doc)
        return doc.render()
    }

    class Doc {
        private val xml = StringBuilder()
        private val media = ArrayList<Pair<String, ByteArray>>()
        private var imgSeq = 0

        fun title(text: String) = para(text, bold = true, size = 30, align = "center", color = ACCENT)
        fun subtitle(text: String) = para(text, bold = false, size = 19, align = "center", color = GRAY)
        fun heading(text: String) = para(text, bold = true, size = 23, align = "left", color = ACCENT)
        fun body(text: String) = para(text, bold = false, size = 21, align = "left", color = INK)
        fun small(text: String) = para(text, bold = false, size = 18, align = "left", color = GRAY)
        fun verdict(text: String, colorHex: String) =
            para(text, bold = true, size = 25, align = "left", color = colorHex)
        fun spacer() = para("", bold = false, size = 12, align = "left", color = INK)

        /** 两列键值表：左列键（浅紫底），右列值。 */
        fun keyValue(rows: List<Pair<String, String>>) =
            table(header = listOf("项目", "内容"), rows = rows.map { listOf(it.first, it.second) },
                widths = listOf(0.30, 0.70))

        /** 通用表格：首行为表头（浅紫底加粗），可指定各列宽度比例。 */
        fun table(header: List<String>, rows: List<List<String>>, widths: List<Double>? = null) {
            val w = widths ?: List(header.size) { 1.0 / header.size }
            val pageW = 9350.0 // A4 可用宽度 (dxa)
            val cw = w.map { (pageW * it).toInt() }
            xml.append("<w:tbl><w:tblPr>")
            xml.append("<w:tblW w:w=\"${pageW.toInt()}\" w:type=\"dxa\"/>")
            xml.append("<w:tblBorders>")
            for (b in listOf("top", "left", "bottom", "right", "insideH", "insideV")) {
                xml.append("<w:$b w:val=\"single\" w:sz=\"4\" w:space=\"0\" w:color=\"BFBFBF\"/>")
            }
            xml.append("</w:tblBorders></w:tblPr>")
            xml.append("<w:tblGrid>")
            for (wv in cw) xml.append("<w:gridCol w:w=\"$wv\"/>")
            xml.append("</w:tblGrid>")

            (listOf(header) + rows).forEachIndexed { idx, row ->
                val isHeader = idx == 0
                xml.append("<w:tr>")
                row.forEachIndexed { ci, cell ->
                    xml.append("<w:tc><w:tcPr>")
                    xml.append("<w:tcW w:w=\"${cw[ci]}\" w:type=\"dxa\"/>")
                    if (isHeader) xml.append("<w:shd w:val=\"clear\" w:fill=\"$ACCENT_SOFT\"/>")
                    xml.append("</w:tcPr><w:p><w:pPr><w:spacing w:after=\"20\"/></w:pPr>")
                    xml.append("<w:r><w:rPr>")
                    xml.append(rPr(isHeader, 20, if (isHeader) ACCENT else INK))
                    xml.append("</w:rPr><w:t xml:space=\"preserve\">").append(esc(cell)).append("</w:t></w:r>")
                    xml.append("</w:p></w:tc>")
                }
                xml.append("</w:tr>")
            }
            xml.append("</w:tbl>")
            spacer()
        }

        /** 内嵌一张图片（PNG）。widthCm/heightCm 为厘米，绘制尺寸由调用方决定。 */
        fun image(bytes: ByteArray, widthCm: Double, heightCm: Double) {
            imgSeq++
            val id = imgSeq
            val cx = (widthCm * 360000.0).toInt() // 1cm = 360000 EMU
            val cy = (heightCm * 360000.0).toInt()
            media.add("image$id.png" to bytes)
            xml.append("<w:p><w:pPr><w:spacing w:after=\"60\"/></w:pPr><w:r><w:drawing>")
            xml.append("<wp:inline distT=\"0\" distB=\"0\" distL=\"0\" distR=\"0\">")
            xml.append("<wp:extent cx=\"$cx\" cy=\"$cy\"/>")
            xml.append("<wp:docPr id=\"$id\" name=\"chart$id\"/>")
            xml.append("<a:graphic><a:graphicData uri=\"http://schemas.openxmlformats.org/drawingml/2006/picture\">")
            xml.append("<pic:pic><pic:nvPicPr><pic:cNvPr id=\"$id\" name=\"chart$id\"/><pic:cNvPicPr/></pic:nvPicPr>")
            xml.append("<pic:blipFill><a:blip r:embed=\"rIdImg$id\"/><a:stretch><a:fillRect/></a:stretch></pic:blipFill>")
            xml.append("<pic:spPr><a:xfrm><a:off x=\"0\" y=\"0\"/><a:ext cx=\"$cx\" cy=\"$cy\"/></a:xfrm>")
            xml.append("<a:prstGeom prst=\"rect\"><a:avLst/></a:prstGeom></pic:spPr>")
            xml.append("</pic:pic></a:graphicData></a:graphic></wp:inline></w:drawing></w:r></w:p>")
        }

        private fun rPr(bold: Boolean, size: Int, color: String): String {
            val b = StringBuilder()
            b.append("<w:rFonts w:ascii=\"Microsoft YaHei\" w:hAnsi=\"Microsoft YaHei\" w:eastAsia=\"Microsoft YaHei\"/>")
            if (bold) b.append("<w:b/>")
            b.append("<w:color w:val=\"$color\"/>")
            b.append("<w:sz w:val=\"$size\"/><w:szCs w:val=\"$size\"/>")
            return b.toString()
        }

        private fun para(text: String, bold: Boolean, size: Int, align: String, color: String) {
            xml.append("<w:p><w:pPr><w:spacing w:after=\"60\"/>")
            xml.append("<w:jc w:val=\"$align\"/></w:pPr>")
            xml.append("<w:r><w:rPr>")
            xml.append(rPr(bold, size, color))
            xml.append("</w:rPr><w:t xml:space=\"preserve\">").append(esc(text)).append("</w:t></w:r>")
            xml.append("</w:p>")
        }

        fun render(): ByteArray {
            val hasMedia = media.isNotEmpty()
            val docNamespaces = if (hasMedia) {
                " xmlns:wp=\"http://schemas.openxmlformats.org/drawingml/2006/wordprocessingDrawing\"" +
                    " xmlns:a=\"http://schemas.openxmlformats.org/drawingml/2006/main\"" +
                    " xmlns:pic=\"http://schemas.openxmlformats.org/drawingml/2006/picture\"" +
                    " xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\""
            } else ""
            val documentXml =
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" +
                    "<w:document xmlns:w=\"http://schemas.openxmlformats.org/wordprocessingml/2006/main\"" + docNamespaces + ">" +
                    "<w:body>" + xml +
                    "<w:sectPr><w:pgSz w:w=\"11906\" w:h=\"16838\"/>" +
                    "<w:pgMar w:top=\"1134\" w:right=\"1134\" w:bottom=\"1134\" w:left=\"1134\" w:header=\"720\" w:footer=\"720\" w:gutter=\"0\"/>" +
                    "</w:sectPr></w:body></w:document>"
            val pngType = if (hasMedia) "<Default Extension=\"png\" ContentType=\"image/png\"/>" else ""
            val contentTypes =
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" +
                    "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">" +
                    "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>" +
                    "<Default Extension=\"xml\" ContentType=\"application/xml\"/>" +
                    pngType +
                    "<Override PartName=\"/word/document.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.wordprocessingml.document.main+xml\"/>" +
                    "</Types>"
            val rels =
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" +
                    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
                    "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"word/document.xml\"/>" +
                    "</Relationships>"
            val docRels = if (hasMedia) {
                "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>\n" +
                    "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">" +
                    media.mapIndexed { i, _ ->
                        "<Relationship Id=\"rIdImg${i + 1}\" " +
                            "Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/image\" " +
                            "Target=\"media/image${i + 1}.png\"/>"
                    }.joinToString("") +
                    "</Relationships>"
            } else null

            val out = ByteArrayOutputStream()
            ZipOutputStream(out).use { zos ->
                zos.putNextEntry(ZipEntry("[Content_Types].xml"))
                zos.write(contentTypes.toByteArray(Charsets.UTF_8))
                zos.closeEntry()
                zos.putNextEntry(ZipEntry("_rels/.rels"))
                zos.write(rels.toByteArray(Charsets.UTF_8))
                zos.closeEntry()
                zos.putNextEntry(ZipEntry("word/document.xml"))
                zos.write(documentXml.toByteArray(Charsets.UTF_8))
                zos.closeEntry()
                if (docRels != null) {
                    zos.putNextEntry(ZipEntry("word/_rels/document.xml.rels"))
                    zos.write(docRels.toByteArray(Charsets.UTF_8))
                    zos.closeEntry()
                }
                for ((fname, bytes) in media) {
                    zos.putNextEntry(ZipEntry("word/media/$fname"))
                    zos.write(bytes)
                    zos.closeEntry()
                }
            }
            return out.toByteArray()
        }

        private fun esc(s: String): String = s
            .filter { it == '\t' || it == '\n' || it == '\r' || it >= ' ' } // 剔除 XML 1.0 非法控制字符（设备字节可能脏）
            .replace("&", "&amp;")
            .replace("<", "&lt;")
            .replace(">", "&gt;")
    }
}
