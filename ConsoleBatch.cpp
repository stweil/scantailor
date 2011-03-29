/*
    Scan Tailor - Interactive post-processing tool for scanned pages.

    ConsoleBatch - Batch processing scanned pages from command line.
    Copyright (C) 2011 Petr Kovar <pejuko@gmail.com>

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program.  If not, see <http://www.gnu.org/licenses/>.
*/

#include <vector>
#include <iostream>
#include <assert.h>

#include "Utils.h"
#include "ProjectPages.h"
#include "PageSelectionAccessor.h"
#include "StageSequence.h"
#include "ProcessingTaskQueue.h"
#include "FileNameDisambiguator.h"
#include "OutputFileNameGenerator.h"
#include "ImageInfo.h"
#include "ImageFileInfo.h"
#include "PageInfo.h"
#include "PageSequence.h"
#include "ImageId.h"
#include "ThumbnailPixmapCache.h"
#include "LoadFileTask.h"
#include "ProjectWriter.h"
#include "ProjectReader.h"
#include "OrthogonalRotation.h"
#include "SelectedPage.h"

#include "filters/fix_orientation/Settings.h"
#include "filters/fix_orientation/Filter.h"
#include "filters/fix_orientation/Task.h"
#include "filters/fix_orientation/CacheDrivenTask.h"
#include "filters/page_split/Settings.h"
#include "filters/page_split/Filter.h"
#include "filters/page_split/Task.h"
#include "filters/page_split/CacheDrivenTask.h"
#include "filters/deskew/Settings.h"
#include "filters/deskew/Filter.h"
#include "filters/deskew/Task.h"
#include "filters/deskew/CacheDrivenTask.h"
#include "filters/select_content/Settings.h"
#include "filters/select_content/Filter.h"
#include "filters/select_content/Task.h"
#include "filters/select_content/CacheDrivenTask.h"
#include "filters/page_layout/Settings.h"
#include "filters/page_layout/Filter.h"
#include "filters/page_layout/Task.h"
#include "filters/page_layout/CacheDrivenTask.h"
#include "filters/output/Settings.h"
#include "filters/output/Params.h"
#include "filters/output/Filter.h"
#include "filters/output/Task.h"
#include "filters/output/CacheDrivenTask.h"

#include <QMap>
#include <QDomDocument>

#include "ConsoleBatch.h"
#include "CommandLine.h"

ConsoleBatch::ConsoleBatch(std::vector<ImageFileInfo> const& images, QString const& output_directory, Qt::LayoutDirection const layout)
:   batch(true), debug(true),
	m_ptrDisambiguator(new FileNameDisambiguator),
	m_ptrPages(new ProjectPages(images, ProjectPages::AUTO_PAGES, layout))
{
	PageSelectionAccessor const accessor(0);
	m_ptrStages = IntrusivePtr<StageSequence>(new StageSequence(m_ptrPages, accessor));

	setup();

	//m_ptrThumbnailCache = IntrusivePtr<ThumbnailPixmapCache>(new ThumbnailPixmapCache(output_dir+"/cache/thumbs", QSize(200,200), 40, 5));
	m_ptrThumbnailCache = Utils::createThumbnailCache(output_directory);
	m_outFileNameGen = OutputFileNameGenerator(m_ptrDisambiguator, output_directory, m_ptrPages->layoutDirection());
}

ConsoleBatch::ConsoleBatch(QString const project_file)
:   batch(true), debug(true)
{
	QFile file(project_file);
	if (!file.open(QIODevice::ReadOnly)) {
		throw "Unable to open the project file.";
	}

	QDomDocument doc;
	if (!doc.setContent(&file)) {
		throw "The project file is broken.";
	}

	file.close();

	m_ptrReader.reset(new ProjectReader(doc));
	m_ptrPages = m_ptrReader->pages();

	PageSelectionAccessor const accessor(0);
	m_ptrDisambiguator = m_ptrReader->namingDisambiguator();

	m_ptrStages = IntrusivePtr<StageSequence>(new StageSequence(m_ptrPages, accessor));
	m_ptrReader->readFilterSettings(m_ptrStages->filters());

	setup();

	CommandLine cli = CommandLine::get();
	QString output_directory = m_ptrReader->outputDirectory();
	if (!cli.outputDirectory().isEmpty()) {
		output_directory = cli.outputDirectory();
	}

	//m_ptrThumbnailCache = IntrusivePtr<ThumbnailPixmapCache>(new ThumbnailPixmapCache(output_directory+"/cache/thumbs", QSize(200,200), 40, 5));
	m_ptrThumbnailCache = Utils::createThumbnailCache(output_directory);
	m_outFileNameGen = OutputFileNameGenerator(m_ptrDisambiguator, output_directory, m_ptrPages->layoutDirection());
}


BackgroundTaskPtr
ConsoleBatch::createCompositeTask(
		PageInfo const& page,
		int const last_filter_idx)
{
	IntrusivePtr<fix_orientation::Task> fix_orientation_task;
	IntrusivePtr<page_split::Task> page_split_task;
	IntrusivePtr<deskew::Task> deskew_task;
	IntrusivePtr<select_content::Task> select_content_task;
	IntrusivePtr<page_layout::Task> page_layout_task;
	IntrusivePtr<output::Task> output_task;
	
	if (batch) {
		debug = false;
	}

	if (last_filter_idx >= m_ptrStages->outputFilterIdx()) {
		output_task = m_ptrStages->outputFilter()->createTask(
			page.id(), m_ptrThumbnailCache, m_outFileNameGen, batch, debug
		);
		debug = false;
	}
	if (last_filter_idx >= m_ptrStages->pageLayoutFilterIdx()) {
		page_layout_task = m_ptrStages->pageLayoutFilter()->createTask(
			page.id(), output_task, batch, debug
		);
		debug = false;
	}
	if (last_filter_idx >= m_ptrStages->selectContentFilterIdx()) {
		select_content_task = m_ptrStages->selectContentFilter()->createTask(
			page.id(), page_layout_task, batch, debug
		);
		debug = false;
	}
	if (last_filter_idx >= m_ptrStages->deskewFilterIdx()) {
		deskew_task = m_ptrStages->deskewFilter()->createTask(
			page.id(), select_content_task, batch, debug
		);
		debug = false;
	}
	if (last_filter_idx >= m_ptrStages->pageSplitFilterIdx()) {
		page_split_task = m_ptrStages->pageSplitFilter()->createTask(
			page, deskew_task, batch, debug
		);
		debug = false;
	}
	if (last_filter_idx >= m_ptrStages->fixOrientationFilterIdx()) {
		fix_orientation_task = m_ptrStages->fixOrientationFilter()->createTask(
			page.id(), page_split_task, batch
		);
		debug = false;
	}
	assert(fix_orientation_task);
	
	return BackgroundTaskPtr(
		new LoadFileTask(
			BackgroundTask::BATCH,
			page, m_ptrThumbnailCache, m_ptrPages, fix_orientation_task
		)
	);
}


// process the image vector **images** and save output to **output_dir**
void
ConsoleBatch::process()
{
	CommandLine cli = CommandLine::get();

	int startFilterIdx = m_ptrStages->selectContentFilterIdx();
	if (cli.contains("start-filter")) {
		int sf = cli.getStartFilterIdx();
		if (sf<0 || sf>=m_ptrStages->filters().size())
			throw "Start filter out of range";
		startFilterIdx = sf;
	}

	int endFilterIdx = m_ptrStages->outputFilterIdx();
	if (cli.contains("end-filter")) {
		int ef = cli.getEndFilterIdx();
		if (ef<0 || ef>=m_ptrStages->filters().size())
			throw "End filter out of range";
		endFilterIdx = ef;
	}

	for (int j=startFilterIdx; j<=endFilterIdx; j++) {
		if (cli.contains("verbose"))
			std::cout << "Filter: " << (j+1) << "\n";

		PageSequence page_sequence = m_ptrPages->toPageSequence(PAGE_VIEW);
		for (unsigned i=0; i<page_sequence.numPages(); i++) {
			PageInfo page = page_sequence.pageAt(i);
			if (cli.contains("verbose"))
				std::cout << "\tProcessing: " << page.imageId().filePath().toAscii().constData() << "\n";
			BackgroundTaskPtr bgTask = createCompositeTask(page, j);
			(*bgTask)();
		}
	}
}

void
ConsoleBatch::saveProject(QString const project_file)
{
	PageInfo fpage = m_ptrPages->toPageSequence(PAGE_VIEW).pageAt(0);
	SelectedPage sPage(fpage.id(), IMAGE_VIEW);
	ProjectWriter writer(m_ptrPages, sPage, m_outFileNameGen);
	writer.write(project_file, m_ptrStages->filters());
}

void
ConsoleBatch::setup()
{
	IntrusivePtr<fix_orientation::Filter> fix_orientation = m_ptrStages->fixOrientationFilter(); 
	IntrusivePtr<page_split::Filter> page_split = m_ptrStages->pageSplitFilter(); 
	IntrusivePtr<deskew::Filter> deskew = m_ptrStages->deskewFilter(); 
	IntrusivePtr<select_content::Filter> select_content = m_ptrStages->selectContentFilter(); 
	IntrusivePtr<page_layout::Filter> page_layout = m_ptrStages->pageLayoutFilter(); 
	IntrusivePtr<output::Filter> output = m_ptrStages->outputFilter(); 

	CommandLine cli = CommandLine::get();
	QMap<QString, float> img_cache;

	std::set<PageId> allPages = m_ptrPages->toPageSequence(PAGE_VIEW).selectAll();
	for (std::set<PageId>::iterator i=allPages.begin(); i!=allPages.end(); i++) {
		PageId page = *i;

		OrthogonalRotation rotation;
		// FIX ORIENTATION FILTER
		if (cli.contains("orientation")) {
			switch(cli.getOrientation()) {
				case CommandLine::LEFT:
					rotation.prevClockwiseDirection();
					break;
				case CommandLine::RIGHT:
					rotation.nextClockwiseDirection();
					break;
				case CommandLine::UPSIDEDOWN:
					rotation.nextClockwiseDirection();
					rotation.nextClockwiseDirection();
					break;
				default:
					break;
			}
			fix_orientation->getSettings()->applyRotation(page.imageId(), rotation);
		}
	
		// DESKEW FILTER
		if (cli.contains("rotate") || cli.contains("deskew")) {
			double angle = 0.0;
			if (cli.contains("rotate"))
				angle = cli.getDeskewAngle();
			deskew::Dependencies deps(QPolygonF(), rotation);
			deskew::Params params(angle, deps, MODE_MANUAL);
			deskew->getSettings()->setPageParams(page, params);
		}
	
		// PAGE SPLIT
		if (cli.contains("layout")) {
			page_split->getSettings()->setLayoutTypeForAllPages(cli.getLayout());
		}

		// SELECT CONTENT FILTER
		if (cli.contains("content-box")) {
			QRectF rect(cli.getContentRect());
			QSizeF size_mm(rect.width(), rect.height());
			select_content::Dependencies deps;
			select_content::Params params(rect, size_mm, deps, MODE_MANUAL);
			select_content->getSettings()->setPageParams(page, params);
		}

		// PAGE LAYOUT FILTER
		page_layout::Alignment alignment = cli.getAlignment();
		if (cli.containsMargins())
			page_layout->getSettings()->setHardMarginsMM(page, cli.getMargins());
		if (cli.containsAlignment())
			page_layout->getSettings()->setPageAlignment(page, alignment);

		// OUTPUT FILTER
		output::Params params(output->getSettings()->getParams(page));
		if (cli.containsOutputDpi()) {
			Dpi outputDpi = cli.getOutputDpi();
			params.setOutputDpi(outputDpi);
		}

		output::ColorParams colorParams = params.colorParams();
		if (cli.contains("color-mode"))
			colorParams.setColorMode(cli.getColorMode());

		if (cli.contains("white-margins") || cli.contains("normalize-illumination")) {
			output::ColorGrayscaleOptions cgo;
			if (cli.contains("white-margins"))
				cgo.setWhiteMargins(true);
			if (cli.contains("normalize-illumination"))
				cgo.setNormalizeIllumination(true);
			colorParams.setColorGrayscaleOptions(cgo);
		}

		if (cli.contains("threshold")) {
			output::BlackWhiteOptions bwo;
			bwo.setThresholdAdjustment(cli.getThreshold());
			colorParams.setBlackWhiteOptions(bwo);
		}

		params.setColorParams(colorParams);

		if (cli.contains("despeckle"))
			params.setDespeckleLevel(cli.getDespeckleLevel());

		if (cli.contains("dewarping"))
			params.setDewarpingMode(cli.getDewarpingMode());
		if (cli.contains("depth-perception"))
			params.setDepthPerception(cli.getDepthPerception());

		output->getSettings()->setParams(page, params);
	}
}