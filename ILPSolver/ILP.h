/* 
 * Copyright (c) 2014 Ignacio Avas
 * For the full copyright and license information, please view the LICENSE.txt
 * file that was distributed with this source code.
*/
#pragma once

#include "scene/Scene.h"
#include "renderer/PMOptixRenderer.h"
#include "Configuration.h"
#include <QVector>
#include <QDir>

class Logger;
class QFile;
class QString;
class Condition;
class OptimizationFunction;
class Evaluation;
class QDomDocument;
class Configuration;

class ILP
{
private:
	static const QString logFileName;
public:
	ILP();
	static ILP fromFile(Logger *logger, const QString& filePath, PMOptixRenderer *renderer);
	void optimize();
private:
	// scene reading
	void readScene(QFile &file, const QString& fileName);
	void readConditions(QDomDocument& doc);
	void readOptimizationFunction(QDomDocument& doc);
	void readOutputPath(const QString &fileName, QDomDocument& doc);

	// misc
	QString getImageFileName();
	QString getImageFileNameSolution(int solutionNum);
	
	// logging
	void cleanOutputDir();
	void logIterationHeader();
	void logIterationResults(Evaluation *evaluation);

	// optimization
	Configuration processInitialConfiguration();
	void findFirstImprovement(QVector<Configuration> &currentEvals, float radius);
	QVector<ConditionPosition *> pushMoveToNeighbourhoodAll(int retries, float radius);
	void popMoveAll();
	void logBestConfigurations(QVector<Configuration> &bestConfigurations);
	

	bool inited;
	Scene *scene;
	QVector<Condition *> conditions;
	OptimizationFunction *optimizationFunction;
	PMOptixRenderer *renderer;
	int currentIteration;
	Logger *logger;
	QDir outputDir;
};